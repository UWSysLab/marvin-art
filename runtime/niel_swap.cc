#include "niel_swap.h"

#include "gc/collector/garbage_collector.h"
#include "gc/heap.h"
#include "gc/space/space-inl.h"
#include "gc/task_processor.h"
#include "mirror/object.h"
#include "mirror/object-inl.h"
#include "niel_common.h"
#include "niel_scoped_timer.h"
#include "niel_stub-inl.h"
#include "runtime.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <queue>
#include <set>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <vector>

namespace art {

namespace niel {

namespace swap {

/* Constants */
const double COMPACT_THRESHOLD = 0.25;
const int BG_MARK_SWEEPS_BEFORE_SEMI_SPACE = 4;
const uint64_t WRITE_TASK_FG_MAX_DURATION = 300000000; // ns
const uint64_t WRITE_TASK_BG_MAX_DURATION = 300000000; // ns
const uint64_t WRITE_TASK_FG_WAIT_TIME = 3000000000; // ns
const uint64_t WRITE_TASK_BG_WAIT_TIME = 10000000000; // ns

// Return values of writeToSwapFile().
const int SWAPFILE_WRITE_OK = 0;
const int SWAPFILE_WRITE_GARBAGE = 1;
const int SWAPFILE_WRITE_NULL_CLASS = 2;
const int SWAPFILE_WRITE_RESIZED = 3;

/* Internal functions */
void scheduleNextTask(Thread * self, bool ioError);
gc::TaskProcessor * getTaskProcessorChecked();
void FreeFromRosAllocSpace(Thread * self, gc::Heap * heap, mirror::Object * obj)
        SHARED_REQUIRES(Locks::mutator_lock_);
void FreeFromLargeObjectSpace(Thread * self, gc::Heap * heap, mirror::Object * obj)
        SHARED_REQUIRES(Locks::mutator_lock_);
void debugPrintDataStructureInfo(Thread * self, const std::string & message);
void dumpObject(mirror::Object * obj);

/*
 * Writes a snapshot of the given object to the swap file, overwriting the object's
 * previous snapshot in the swap file if one exists. Clears the object's dirty bit.
 * Grabs the swapFileMutex and swapFileMapsMutex during execution.
 */
int writeToSwapFile(Thread * self, gc::Heap * heap, mirror::Object * object, bool * ioError)
        SHARED_REQUIRES(Locks::mutator_lock_);

/*
 * Allocates space for the object in memory and copies the object from the swap
 * file into memory.
 */
mirror::Object * swapInObject(Thread * self, gc::Heap * heap, Stub * stub,
                              std::streampos objOffset, size_t objSize)
        SHARED_REQUIRES(Locks::mutator_lock_);

/*
 * Updates swap data structures based on a mapping from "old pointers" to "new
 * pointers" so that any old pointers in the data structures are replaced with
 * the corresponding new pointers.
 *
 * More precisely, for each key-value pair (a,b) in addrTable, this function:
 *
 * 1) Replaces any key-value pair (a,c) in objectOffsetMap or objectSizeMap
 * with the new entry (b,c).
 *
 * 2) Replaces any key-value pair (a,c) in objectStubMap with the new entry
 * (b,c), and replaces any key-value pair (d,a) with the new entry (d,b).
 * (If there are two key-value pairs (a,b) and (e,f) in addrTable and a
 * key-value pair (a,e) in objectStubMap, this function replaces it with the
 * single new key-value pair (b,f).)
 *
 * If removeUntouchedRosAlloc is true, this function removes all entries in the
 * data structures corresponding to pointers that are not in the given mapping
 * and not in the large object space. The purpose of this parameter is to allow
 * this function to clean up entries corresponding to objects freed by the
 * SemiSpace garbage collector.
 *
 * More precisely, given that object a is not in the large object space, this
 * function:
 *
 * 1) Removes all pairs (a,c) from objectOffsetMap and objectSizeMap that did
 * not have a corresponding pair (a,b) in addrTable.
 *
 * 2) Removes all pairs (a,c) from objectStubMap that did not have a
 * corresponding pair (a,b) in addrTable.
 */
void replaceDataStructurePointers(Thread * self, const std::map<void *, void *> & addrTable,
                                  bool removeUntouchedRosAlloc);

/*
 * Copies a file. Returns true on success and false on error. The caller is
 * responsible for making sure that all open file descriptors to both the
 * source and destination files are closed.
 */
bool copyFile(const std::string & fromFileName, const std::string & toFileName);

/*
 * Checks to make sure that the swap file contains every object at the correct
 * position. This function exclusively holds the swapFileMutex for a long time!
 * Use it only in debug builds.
 */
void validateSwapFile(Thread * self);

/*
 * Walk all of the memory spaces and replace any references to swapped-out
 * objects with references to their corresponding stubs.
 */
void patchStubReferences(Thread * self, gc::Heap * heap) REQUIRES(Locks::mutator_lock_);

/*
 * Determines whether an object and the objects referenced by it should be
 * excluded from being swapped out. The caller is responsible for setting and
 * clearing the object's IgnoreReadFlag before and after calling this function.
 *
 * Note: currently, this method does not exclude any objects from being swapped
 * out, but I am leaving this code path in place in case we need to exclude
 * objects in the future.
 */
bool shouldExcludeObjectAndMembers(mirror::Object * obj) SHARED_REQUIRES(Locks::mutator_lock_);

/* Variables */
Mutex writeQueueMutex("WriteQueueMutex", kLoggingLock);
ReaderWriterMutex objectStubMapMutex("ObjectStubMapMutex", kLoggingLock);
ReaderWriterMutex swapFileMapsMutex("SwapFileMapsMutex", kDefaultMutexLevel);
// Holding the swapFileMutex while reading the swap file always guarantees that
// reads return valid data, and it guarantees that reads reflect the latest
// writes iff the WriteTask always runs on the same thread as swap file
// compaction (as it does for marlin builds running on a Pixel XL).
Mutex swapFileMutex("SwapFileMutex", kLoggingLock);

/*
 * Not locked but assumed to be only touched in one thread (because all Tasks,
 * including GC, run on the same fixed thread)
 */
uint32_t pid = 0;
std::map<void *, void *> remappingTable;
std::map<void *, void *> semiSpaceRemappingTable; //TODO: combine with normal remapping table?
std::set<mirror::Object *> swapOutSet;
bool swappingOutObjects = false;
bool doingSwapInCleanup = false;
int bgMarkSweepCounter = 0;
int freedObjects = 0;
long freedSize = 0;
int swapfileObjects = 0;
int swapfileSize = 0;

/*
 * Not locked but only modified in Heap::UpdateProcessState() and read in Tasks
 */
bool appInForeground = true;

/*
 * Not locked but will only ever switch from false to true, and it shouldn't
 * cause any correctness issues if an app thread incorrectly reads it as false
 * for a little while after it has changed
 */
bool swapEnabled = false;

/*
 * Locked by swapFileMutex
 */
std::fstream swapfile;

/*
 * Locked by swapFileMapsMutex
 */
// These maps describe the position and size of checkpointed objects in the
// swap file. If a stub has been created for an object, the key associated with
// the object's information is the stub pointer. If no stub has been created
// yet, the key is a pointer to the object itself.
std::map<void *, std::streampos> objectOffsetMap;
std::map<void *, size_t> objectSizeMap;

/*
 * Locked by writeQueueMutex
 */
std::vector<mirror::Object *> writeQueue;
std::set<mirror::Object *> writeSet; // prevents duplicate entries in writeQueue

/*
 * Locked by objectStubMapMutex
 */
// The objectStubMap maps every memory-resident "swap candidate" object that
// has a stub to its stub.
std::map<mirror::Object *, Stub *> objectStubMap;

class WriteTask : public gc::HeapTask {
  public:
    WriteTask(uint64_t target_time) : gc::HeapTask(target_time) { }

    virtual void Run(Thread * self) {
        CHECK(swapEnabled);

        bool done = false;
        uint64_t startTime = NanoTime();
        bool ioError = false;

        int numGarbageObjects = 0;
        int numNullClasses = 0;
        int numResizedObjects = 0;

        gc::Heap * heap = getHeapChecked();

        int queueSize = 0;

        Locks::mutator_lock_->ReaderLock(self);
        while (!done) {
            mirror::Object * object = nullptr;

            writeQueueMutex.ExclusiveLock(self);
            queueSize = writeQueue.size();
            if (queueSize == 0) {
                done = true;
            }
            else {
                object = writeQueue.front();
                writeQueue.erase(writeQueue.begin());
                writeSet.erase(object);
            }
            writeQueueMutex.ExclusiveUnlock(self);

            if (object != nullptr && !object->GetNoSwapFlag()) {
                int writeResult = writeToSwapFile(self, heap, object, &ioError);
                if (writeResult == SWAPFILE_WRITE_GARBAGE) {
                    numGarbageObjects++;
                }
                if (writeResult == SWAPFILE_WRITE_NULL_CLASS) {
                    numNullClasses++;
                }
                if (writeResult == SWAPFILE_WRITE_RESIZED) {
                    numResizedObjects++;
                }
            }

            if (ioError) {
                done = true;
            }

            uint64_t currentTime = NanoTime();
            uint64_t maxDuration = appInForeground ? WRITE_TASK_FG_MAX_DURATION
                                                   : WRITE_TASK_BG_MAX_DURATION;
            if (currentTime - startTime > maxDuration) {
                done = true;
            }
        }
        Locks::mutator_lock_->ReaderUnlock(self);

        LOG(INFO) << "NIEL done writing objects in WriteTask; " << queueSize
                  << " objects still in queue; swap file has " << swapfileObjects
                  << " objects, size " << swapfileSize;
        if (numGarbageObjects > 0 || numNullClasses > 0 || numResizedObjects > 0) {
            LOG(ERROR) << "NIELERROR WriteTask irregularities: " << numGarbageObjects
                       << " garbage objects, " << numNullClasses << " null classes, "
                       << numResizedObjects << " resized objects";
        }

        if ((double)freedSize / swapfileSize > COMPACT_THRESHOLD) {
            CompactSwapFile(self);
            validateSwapFile(self);
        }

        scheduleNextTask(self, ioError);
    }
};

void SetInForeground(bool inForeground) {
    appInForeground = inForeground;
    if (inForeground) {
        bgMarkSweepCounter = 0;
    }
}

void scheduleNextTask(Thread * self, bool ioError) {
    uint64_t nanoTime = NanoTime();
    uint64_t waitTime = appInForeground ? WRITE_TASK_FG_WAIT_TIME : WRITE_TASK_BG_WAIT_TIME;
    uint64_t targetTime = nanoTime + waitTime;
    gc::Heap * curHeap = getHeapChecked();
    gc::TaskProcessor * taskProcessor = getTaskProcessorChecked();
    if (taskProcessor != nullptr && taskProcessor->IsRunning()) {
        if (ioError) {
            LOG(INFO) << "NIEL not scheduling WriteTask again due to IO error";
        }
        else {
            if (appInForeground) {
                curHeap->RequestConcurrentGC(self, true);
            }
            else {
                if (bgMarkSweepCounter >= BG_MARK_SWEEPS_BEFORE_SEMI_SPACE) {
                    curHeap->PerformHomogeneousSpaceCompact();
                    bgMarkSweepCounter = 0;
                }
                else {
                    curHeap->RequestConcurrentGC(self, true);
                    bgMarkSweepCounter++;
                }
            }
            taskProcessor->AddTask(self, new WriteTask(targetTime));
        }
    }
}

int writeToSwapFile(Thread * self, gc::Heap * heap, mirror::Object * object, bool * ioError) {
    int result = SWAPFILE_WRITE_OK;
    *ioError = false;

    CHECK(   heap->GetRosAllocSpace()->Contains(object)
          || heap->GetLargeObjectsSpace()->Contains(object))
        << " addr " << object;

    bool validObject = false;
    if (object != nullptr) {
        object->SetIgnoreReadFlag();
        if (object->GetClass() != nullptr) {
            validObject = true;
        }
        else {
            result = SWAPFILE_WRITE_NULL_CLASS;
        }
        object->ClearIgnoreReadFlag();
    }

    if (validObject) {
        object->SetIgnoreReadFlag();
        size_t objectSize = object->SizeOf();
        object->ClearIgnoreReadFlag();

        char * objectData = new char[objectSize];
        object->ClearDirtyBit();
        std::memcpy(objectData, object, objectSize);

        /*
         * The code below is a bit convoluted because the key used to look up
         * an object's data in the objectOffsetMap and objectSizeMap differs
         * depending on whether the object has a stub. If the object has a
         * stub, its stub is the key; if the object does not have a stub (i.e.,
         * it was selected for swapping since the last background transition),
         * the object itself is the key.
         */
        bool objectInSwapFileMaps = false;
        Stub * stub = nullptr;

        objectStubMapMutex.SharedLock(self);
        if (objectStubMap.find(object) != objectStubMap.end()) {
            stub = objectStubMap[object];
        }
        objectStubMapMutex.SharedUnlock(self);

        swapFileMapsMutex.SharedLock(self);
        objectInSwapFileMaps = (objectOffsetMap.find(object) != objectOffsetMap.end());
        swapFileMapsMutex.SharedUnlock(self);

        CHECK(!(stub != nullptr && objectInSwapFileMaps));

        void * swapStateKey = nullptr;
        bool inSwapFile = false;
        if (stub != nullptr) {
            swapStateKey = stub;
            inSwapFile = true;
        }
        else if (objectInSwapFileMaps) {
            swapStateKey = object;
            inSwapFile = true;
        }
        else {
            swapStateKey = object;
            inSwapFile = false;
        }

        if (!inSwapFile) {
            swapFileMutex.ExclusiveLock(self);
            std::streampos offset = swapfile.tellp();
            swapfile.write(objectData, objectSize);
            bool writeError = checkStreamError(swapfile,
                    "writing object to swapfile for the first time in WriteTask");
            if (writeError) {
                *ioError = true;
            }
            swapFileMutex.ExclusiveUnlock(self);

            swapFileMapsMutex.ExclusiveLock(self);
            objectOffsetMap[swapStateKey] = offset;
            objectSizeMap[swapStateKey] = objectSize;
            swapFileMapsMutex.ExclusiveUnlock(self);

            swapfileObjects++;
            swapfileSize += objectSize;
        }
        else {
            swapFileMapsMutex.SharedLock(self);
            std::streampos offset = objectOffsetMap[swapStateKey];
            size_t size = objectSizeMap[swapStateKey];
            swapFileMapsMutex.SharedUnlock(self);

            if (size != objectSize) {
                result = SWAPFILE_WRITE_RESIZED;
            }
            else {
                swapFileMutex.ExclusiveLock(self);
                std::streampos curpos = swapfile.tellp();
                swapfile.seekp(offset);
                swapfile.write(objectData, objectSize);
                bool writeError = checkStreamError(swapfile,
                        "writing object to swapfile again in WriteTask");
                if (writeError) {
                    *ioError = true;
                }
                swapfile.seekp(curpos);
                swapFileMutex.ExclusiveUnlock(self);
            }
        }
        delete[] objectData;
    }
    return result;
}

bool shouldExcludeObjectAndMembers(mirror::Object * obj ATTRIBUTE_UNUSED) {
    return false;
}

class ExcludeVisitor {
  public:
    void operator()(mirror::Object * obj,
                    MemberOffset offset,
                    bool is_static ATTRIBUTE_UNUSED) const
            SHARED_REQUIRES(Locks::mutator_lock_) {
        mirror::Object * ref = obj->GetFieldObject<mirror::Object>(offset);
        if (ref != nullptr) {
            if (ref->GetStubFlag()) {
                LOG(ERROR) << "NIELERROR ref " << ref << " of object " << obj
                           << " that should be excluded from swapping was already swapped out";
            }
            else {
                ref->SetNoSwapFlag();
            }
        }
    }

    void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED) const {}

    void VisitRoot(mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED) const {}
};

class DummyReferenceVisitor {
  public:
    void operator()(mirror::Class* klass ATTRIBUTE_UNUSED,
                    mirror::Reference* ref ATTRIBUTE_UNUSED) const { }
};

// Based on MarkVisitor in runtime/gc/collector/mark_sweep.cc
class PatchVisitor {
  public:
    void operator()(mirror::Object * obj,
                    MemberOffset offset,
                    bool is_static ATTRIBUTE_UNUSED) const
            SHARED_REQUIRES(Locks::mutator_lock_) {
        mirror::Object * ref = obj->GetFieldObject<mirror::Object>(offset);
        if (remappingTable.count(ref)) {
            obj->SetFieldObject<false>(offset, (mirror::Object *)remappingTable[ref]);
        }
    }

    void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED) const {}

    void VisitRoot(mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED) const {}
};


// Method signature from MarkSweep::DelayReferenceReferentVisitor in
// runtime/gc/collector/mark_sweep.cc.
class PatchReferenceVisitor {
  public:
    void operator()(mirror::Class* klass ATTRIBUTE_UNUSED,
                    mirror::Reference* ref) const
            SHARED_REQUIRES(Locks::mutator_lock_) {
        mirror::Object * referent = ref->GetReferent();
        if (remappingTable.count(referent)) {
            ref->SetReferent<false>((mirror::Object *)remappingTable[referent]);
        }
    }
};

void PatchCallback(void * start, void * end ATTRIBUTE_UNUSED, size_t num_bytes,
                  void * callback_arg ATTRIBUTE_UNUSED)
        REQUIRES(Locks::mutator_lock_) {
    if (start == nullptr || num_bytes == 0) {
        return;
    }

    mirror::Object * obj = (mirror::Object *)start;
    if (obj->GetStubFlag()) {
        Stub * stub = (Stub *)obj;
        mirror::Object * swappedInObj = stub->GetObjectAddress();
        if (remappingTable.count(swappedInObj)) {
            stub->SetObjectAddress((mirror::Object *)remappingTable[swappedInObj]);
        }
        for (int i = 0; i < stub->GetNumRefs(); i++) {
            mirror::Object * ref = stub->GetReference(i);
            if (remappingTable.count(ref)) {
                stub->SetReference(i, (mirror::Object *)remappingTable[ref]);
            }
        }
    }
    else {
        PatchVisitor visitor;
        PatchReferenceVisitor referenceVisitor;
        obj->VisitReferences(visitor, referenceVisitor);
    }
}

class GlobalRefRootVisitor : public RootVisitor {
    void VisitRoots(mirror::Object*** roots, size_t count,
                    const RootInfo & info ATTRIBUTE_UNUSED)
            SHARED_REQUIRES(Locks::mutator_lock_) {
        for (size_t i = 0; i < count; i++) {
            mirror::Object * oldRef = *roots[i];
            if (remappingTable.count(oldRef)) {
                *roots[i] = (mirror::Object *)remappingTable[oldRef];
                LOG(INFO) << "NIEL VisitRoots patching ref " << oldRef << " to " << remappingTable[oldRef];
            }
        }
    }

    void VisitRoots(mirror::CompressedReference<mirror::Object>** roots, size_t count,
                    const RootInfo & info ATTRIBUTE_UNUSED)
            SHARED_REQUIRES(Locks::mutator_lock_) {
        for (size_t i = 0; i < count; i++) {
            mirror::Object * oldRef = roots[i]->AsMirrorPtr();
            if (remappingTable.count(oldRef)) {
                roots[i]->Assign((mirror::Object *)remappingTable[oldRef]);
                LOG(INFO) << "NIEL VisitRoots patching ref " << oldRef << " to " << remappingTable[oldRef];
            }
        }
    }
};

void patchStubReferences(Thread * self, gc::Heap * heap) {
    ScopedTimer timer("patching stub references");

    gc::space::LargeObjectSpace * largeObjectSpace = heap->GetLargeObjectsSpace();
    largeObjectSpace->Walk(&PatchCallback, nullptr);

    gc::space::RosAllocSpace * rosAllocSpace = heap->GetRosAllocSpace();
    rosAllocSpace->Walk(&PatchCallback, nullptr);

    GlobalRefRootVisitor visitor;
    Runtime::Current()->VisitRoots(&visitor, kVisitRootFlagAllRoots);

    replaceDataStructurePointers(self, remappingTable, false);
}

//TODO: figure out whether it's better to grab the lock once or keep grabbing
//      and releasing
void replaceDataStructurePointers(Thread * self, const std::map<void *, void *> & addrTable,
                                  bool removeUntouchedRosAlloc) {
    swapFileMapsMutex.ExclusiveLock(self);

    std::set<void *> swapFileMapsRemoveSet;
    for (auto it = objectOffsetMap.begin(); it != objectOffsetMap.end(); it++) {
        if (!addrTable.count(it->first)) {
            // Assumes that a pointer not in the large object space was in the
            // old RosAlloc space.
            if (!getHeapChecked()->GetLargeObjectsSpace()
                                 ->Contains((mirror::Object *)it->first)) {
              swapFileMapsRemoveSet.insert(it->first);
            }
        }
    }

    for (auto it = addrTable.begin(); it != addrTable.end(); it++) {
        void * originalPtr = it->first;
        void * newPtr = it->second;

        CHECK_EQ(objectOffsetMap.count(originalPtr), objectSizeMap.count(originalPtr));

        if (objectOffsetMap.count(originalPtr)) {
            auto oomIt = objectOffsetMap.find(originalPtr);
            objectOffsetMap[newPtr] = oomIt->second;
            objectOffsetMap.erase(oomIt);

            auto osmIt = objectSizeMap.find(originalPtr);
            objectSizeMap[newPtr] = osmIt->second;
            objectSizeMap.erase(osmIt);
        }
    }

    if (removeUntouchedRosAlloc) {
        for (auto it = swapFileMapsRemoveSet.begin(); it != swapFileMapsRemoveSet.end(); it++) {
            objectOffsetMap.erase(*it);
            objectSizeMap.erase(*it);
        }
    }

    swapFileMapsMutex.ExclusiveUnlock(self);

    objectStubMapMutex.ExclusiveLock(self);
    std::map<mirror::Object *, Stub *> newObjectStubMap;
    int debugCounter = 0;
    for (auto it = objectStubMap.begin(); it != objectStubMap.end(); it++) {
        mirror::Object * oldKey = it->first;
        Stub * oldVal = it->second;
        mirror::Object * newKey = nullptr;
        Stub * newVal = nullptr;
        if (addrTable.count(oldKey)) {
            newKey = (mirror::Object *)addrTable.at(oldKey);
        }
        else {
            newKey = oldKey;
        }
        if (addrTable.count(oldVal)) {
            newVal = (Stub *)addrTable.at(oldVal);
        }
        else {
            newVal = oldVal;
        }
        // As above, assumes that a pointer that is not in the large object
        // space was in the old RosAlloc space
        bool shouldRemove = (   !addrTable.count(oldKey)
                             && !getHeapChecked()->GetLargeObjectsSpace()->Contains(oldKey));
        if (!removeUntouchedRosAlloc || !shouldRemove) {
          newObjectStubMap[newKey] = newVal;
        }
        else {
          debugCounter++;
        }
    }
    objectStubMap = newObjectStubMap;
    objectStubMapMutex.ExclusiveUnlock(self);
}

void FreeFromRosAllocSpace(Thread * self, gc::Heap * heap, mirror::Object * obj) {
    heap->GetRosAllocSpace()->GetLiveBitmap()->Clear(obj);
    heap->GetRosAllocSpace()->FreeList(self, 1, &obj);
}

void FreeFromLargeObjectSpace(Thread * self, gc::Heap * heap, mirror::Object * obj) {
    heap->GetLargeObjectsSpace()->GetLiveBitmap()->Clear(obj);
    heap->GetLargeObjectsSpace()->Free(self, obj);
}

void SwapObjectsOut(Thread * self, gc::Heap * heap) {
    if (!swapEnabled) {
        return;
    }

    CHECK(remappingTable.size() == 0);

    // Remove any object from swapOutSet whose NoSwapFlag was set since it was
    // added (currently, this only happens because the object was marked for
    // exclusion)
    std::set<mirror::Object *> removeSet;
    for (auto it = swapOutSet.begin(); it != swapOutSet.end(); it++) {
        mirror::Object * obj = *it;
        if (obj->GetNoSwapFlag()) {
            removeSet.insert(obj);
        }
    }
    for (auto it = removeSet.begin(); it != removeSet.end(); it++) {
        swapOutSet.erase(*it);
    }

    // Make sure all objects in swapOutSet have been written to the swap file
    for (auto it = swapOutSet.begin(); it != swapOutSet.end(); it++) {
        mirror::Object * obj = *it;
        if (obj->GetDirtyBit()) {
            bool ioError = false;
            int writeResult = writeToSwapFile(self, heap, obj, &ioError);
            if (ioError || writeResult != SWAPFILE_WRITE_OK) {
                LOG(ERROR) << "NIELERROR error writing object during SwapObjectsOut; result: "
                           << writeResult << " ioError: " << ioError;
            }
        }
    }

    swappingOutObjects = true;
    for (auto it = swapOutSet.begin(); it != swapOutSet.end(); it++) {
        mirror::Object * obj = *it;

        CHECK(   heap->GetRosAllocSpace()->Contains(obj)
              || heap->GetLargeObjectsSpace()->Contains(obj));

        Stub * stub = nullptr;
        objectStubMapMutex.ExclusiveLock(self);
        if (objectStubMap.count(obj)) {
            stub = objectStubMap[obj];
            objectStubMap.erase(obj);
        }
        objectStubMapMutex.ExclusiveUnlock(self);

        // Only allocate a new stub for this object if the object does not
        // already have a stub.
        if (stub != nullptr) {
            //TODO: remove the line below after adding stub updates
            stub->PopulateFrom(obj);
            //TODO: remove the line below after implementing stub restoration in compiled code
            remappingTable[obj] = stub;
        }
        else {
            size_t stubSize = Stub::GetStubSize(obj);
            size_t bytes_allocated;
            size_t usable_size;
            size_t bytes_tl_bulk_allocated;
            mirror::Object * stubData = heap->GetRosAllocSpace()
                                             ->Alloc(self,
                                                     stubSize,
                                                     &bytes_allocated,
                                                     &usable_size,
                                                     &bytes_tl_bulk_allocated);
            CHECK(stubData != nullptr);
            stub = (Stub *)stubData;
            stub->PopulateFrom(obj);

            if (heap->GetLargeObjectsSpace()->Contains(obj)) {
                stub->SetLargeObjectFlag();
            }

            remappingTable[obj] = stub;
        }

        stub->SetObjectAddress(nullptr);

        if (heap->GetLargeObjectsSpace()->Contains(obj)) {
            FreeFromLargeObjectSpace(self, heap, obj);
        }
        else {
            FreeFromRosAllocSpace(self, heap, obj);
        }
    }

    patchStubReferences(self, heap);
    remappingTable.clear();
    swapOutSet.clear();
    swappingOutObjects = false;

    // Clear the write queue (and write set), since after the semi-space GC
    // runs, the pointers in the write queue will be invalid
    writeQueueMutex.ExclusiveLock(self);
    writeQueue.clear();
    writeSet.clear();
    writeQueueMutex.ExclusiveUnlock(self);
}

void RecordForwardedObject(Thread * self, mirror::Object * obj, mirror::Object * forwardAddress) {
    if (!swapEnabled) {
        return;
    }

    swapFileMapsMutex.SharedLock(self);
    if (objectOffsetMap.count(obj)) {
        semiSpaceRemappingTable[obj] = forwardAddress;
    }
    swapFileMapsMutex.SharedUnlock(self);

    objectStubMapMutex.SharedLock(self);
    if (objectStubMap.count(obj)) {
      semiSpaceRemappingTable[obj] = forwardAddress;
    }
    objectStubMapMutex.SharedUnlock(self);
}

void SemiSpaceUpdateDataStructures(Thread * self) {
    if (!swapEnabled) {
        return;
    }

    /*
     * We call replaceDataStructurePointers() with removeUntouchedRosAlloc set
     * to true because we assume that if an object/stub in the old RosAlloc
     * space is present in the objectOffsetMap/objectSizeMap or objectStubMap
     * but missing from the semiSpaceRemappingTable, it is missing because it
     * was freed by the SemiSpace GC.
     */
    replaceDataStructurePointers(self, semiSpaceRemappingTable, true);
    semiSpaceRemappingTable.clear();
}

mirror::Object * swapInObject(Thread * self, gc::Heap * heap, Stub * stub,
                              std::streampos objOffset, size_t objSize) {
    CHECK(stub->GetObjectAddress() == nullptr);

    mirror::Object * newObj = nullptr;
    if (stub->GetLargeObjectFlag()) {
        //TODO: handle allocation failure like Heap::AllocObjectWithAllocator()
        size_t bytesAllocated;
        size_t usableSize;
        size_t bytesTlBulkAllocated;
        newObj = heap->GetLargeObjectsSpace()
                     ->Alloc(self,
                             objSize,
                             &bytesAllocated,
                             &usableSize,
                             &bytesTlBulkAllocated);
    }
    else {
        //TODO: handle allocation failure like Heap::AllocObjectWithAllocator()
        size_t bytesAllocated;
        size_t usableSize;
        size_t bytesTlBulkAllocated;
        newObj = heap->GetRosAllocSpace()
                     ->Alloc(self,
                             objSize,
                             &bytesAllocated,
                             &usableSize,
                             &bytesTlBulkAllocated);
    }
    CHECK(newObj != nullptr);

    swapFileMutex.ExclusiveLock(self);
    std::streampos curPos = swapfile.tellp();
    swapfile.seekg(objOffset);
    swapfile.read((char *)newObj, objSize);
    swapfile.seekg(curPos);
    swapFileMutex.ExclusiveUnlock(self);

    CHECK(newObj->GetDirtyBit() == 0);

    stub->CopyRefsInto(newObj);
    stub->SetObjectAddress(newObj);

    objectStubMapMutex.ExclusiveLock(self);
    CHECK_EQ(objectStubMap.count(newObj), 0u);
    objectStubMap[newObj] = stub;
    objectStubMapMutex.ExclusiveUnlock(self);

    return newObj;
}

//TODO: make sure obj doesn't get freed during GC as long as stub isn't freed,
//      but is freed when stub is freed
void SwapInOnDemand(Stub * stub) {
    CHECK(swapEnabled);

    gc::Heap * heap = getHeapChecked();
    CHECK(heap->GetRosAllocSpace()->Contains((mirror::Object *)stub));

    Thread * self = Thread::Current();

    swapFileMapsMutex.SharedLock(self);
    std::streampos objOffset = objectOffsetMap[stub];
    size_t objSize = objectSizeMap[stub];
    swapFileMapsMutex.SharedUnlock(self);

    swapInObject(self, heap, stub, objOffset, objSize);
}

void SwapObjectsIn(gc::Heap * heap) {
    if (!swapEnabled) {
        return;
    }

    ScopedTimer timer("swapping objects back in");

    Thread * self = Thread::Current();

    swapFileMapsMutex.SharedLock(self);
    for (auto it = objectOffsetMap.begin(); it != objectOffsetMap.end(); it++) {
        mirror::Object * obj = (mirror::Object *)it->first;
        if (obj->GetStubFlag()) {
            Stub * stub = (Stub *)obj;
            CHECK(heap->GetRosAllocSpace()->Contains((mirror::Object *)stub));

            std::streampos objOffset = objectOffsetMap[stub];
            size_t objSize = objectSizeMap[stub];

            // Only swap in object if it wasn't already swapped in on-demand
            mirror::Object * swappedInObj = stub->GetObjectAddress();
            if (swappedInObj == nullptr) {
                swappedInObj = swapInObject(self, heap, stub, objOffset, objSize);
            }
        }
    }
    swapFileMapsMutex.SharedUnlock(self);
}

gc::TaskProcessor * getTaskProcessorChecked() {
    gc::Heap * heap = getHeapChecked();
    if (heap == nullptr) {
        return nullptr;
    }
    return heap->GetTaskProcessor();
}

void GcRecordFree(Thread * self, mirror::Object * object) {
    if (!swapEnabled) {
        return;
    }

    /*
     * The correctness of this check depends on upon several assumptions:
     * 1) Only the garbage collectors and my swapping code call Free() on the
     *    RosAlloc space and large object space.
     * 2) The garbage collectors never run concurrently with any swapping
     *    methods that free objects or stubs as part of swapping (currently,
     *    the only such method is SwapObjectsOut()).
     */
    if (swappingOutObjects) {
        return;
    }

    writeQueueMutex.ExclusiveLock(self);
    auto writeQueuePos = std::find(writeQueue.begin(), writeQueue.end(), object);
    if (writeQueuePos != writeQueue.end()) {
        writeQueue.erase(writeQueuePos);
        writeSet.erase(object);
    }
    writeQueueMutex.ExclusiveUnlock(self);

    swapFileMapsMutex.ExclusiveLock(self);
    if (objectOffsetMap.count(object) && objectSizeMap.count(object)) {
        freedObjects++;
        freedSize += objectSizeMap[object];
        objectOffsetMap.erase(object);
        objectSizeMap.erase(object);
    }
    else if (objectOffsetMap.count(object) || objectSizeMap.count(object)) {
        LOG(ERROR) << "NIELERROR: object in one of the object maps but not the other";
    }
    swapFileMapsMutex.ExclusiveUnlock(self);

    if (swapOutSet.count(object)) {
        swapOutSet.erase(object);
    }

    objectStubMapMutex.ExclusiveLock(self);
    if (objectStubMap.count(object)) {
        objectStubMap.erase(object);
    }
    objectStubMapMutex.ExclusiveUnlock(self);
}

void InitIfNecessary(Thread * self) {
    uint32_t curPid = getpid();
    if (curPid == pid) {
        return;
    }

    gc::TaskProcessor * taskProcessor = getTaskProcessorChecked();
    if (taskProcessor == nullptr || !taskProcessor->IsRunning()) {
        LOG(ERROR) << "NIELERROR failed to init swap since heap's TaskProcessor is null or not "
                   << "ready (or heap is null)";
        return;
    }

    // Once we reach this point, we will not try to init swap again
    // until the next time the PID changes

    pid = curPid;

    swapFileMapsMutex.ExclusiveLock(self);
    objectOffsetMap.clear();
    objectSizeMap.clear();
    swapFileMapsMutex.ExclusiveUnlock(self);

    writeQueueMutex.ExclusiveLock(self);
    writeQueue.clear();
    writeSet.clear();
    writeQueueMutex.ExclusiveUnlock(self);

    std::string packageName = getPackageName();
    std::string swapfilePath("/data/data/" + packageName + "/swapfile");

    if (appOnCommonBlacklist(packageName)) {
        LOG(ERROR) << "NIELERROR stopping swap initialization due to blacklisted app"
                   << " (package name " << packageName << ")";
        return;
    }

    swapFileMutex.ExclusiveLock(self);
    openFile(swapfilePath, swapfile);
    swapfile.write((char *)&pid, 4);
    swapfile.flush();
    bool ioError = checkStreamError(swapfile, "after opening swapfile");
    swapFileMutex.ExclusiveUnlock(self);

    if (ioError) {
        LOG(ERROR) << "NIELERROR not scheduling first WriteTask due to IO error (package name "
                   << packageName << ")";
        return;
    }

    swapEnabled = true;
    taskProcessor->AddTask(Thread::Current(), new WriteTask(NanoTime()));

    LOG(INFO) << "NIEL successfully initialized swap for package " << packageName;
}

bool copyFile(const std::string & fromFileName, const std::string & toFileName) {
    bool copyingError = false;
    int removeRet = remove(toFileName.c_str());
    if (removeRet < 0) {
        LOG(INFO) << "NIEL error removing file (maybe expected): " << toFileName;
    }

    struct stat statBuf;
    int ret = stat(fromFileName.c_str(), &statBuf);
    if (ret < 0) {
        copyingError = true;
    }
    mode_t mode = statBuf.st_mode;
    int fileSize = statBuf.st_size;

    int fromFileFd = open(fromFileName.c_str(), O_RDONLY);
    if (fromFileFd < 0) {
        copyingError = true;
    }
    int toFileFd = open(toFileName.c_str(), O_WRONLY | O_CREAT | O_EXCL);
    if (toFileFd < 0) {
        copyingError = true;
    }
    ret = sendfile(toFileFd, fromFileFd, NULL, fileSize);
    if (ret < 0) {
        copyingError = true;
    }
    ret = close(fromFileFd);
    if (ret < 0) {
        copyingError = true;
    }
    ret = close(toFileFd);
    if (ret < 0) {
        copyingError = true;
    }

    ret = chmod(toFileName.c_str(), mode);
    if (ret < 0) {
        copyingError = true;
    }

    return copyingError;
}

void CompactSwapFile(Thread * self) {
    CHECK(swapEnabled);
    LOG(INFO) << "NIEL compacting swap file";

    std::string swapfilePath("/data/data/" + getPackageName() + "/swapfile");
    std::string oldSwapfilePath("/data/data/" + getPackageName() + "/oldswapfile");
    std::string newSwapfilePath("/data/data/" + getPackageName() + "/newswapfile");
    bool ioError = false;

    freedObjects = 0;
    freedSize = 0;
    swapfileObjects = 0;
    swapfileSize = 0;

    swapFileMutex.ExclusiveLock(self);
    swapfile.close();
    bool copyingError = copyFile(swapfilePath, oldSwapfilePath);
    if (copyingError) {
        LOG(ERROR) << "NIELERROR error copying swap file";
    }
    openFileAppend(swapfilePath, swapfile);
    swapFileMutex.ExclusiveUnlock(self);

    std::fstream oldSwapfile;
    oldSwapfile.open(oldSwapfilePath, std::ios::binary | std::ios::in);
    std::fstream newSwapfile;
    openFile(newSwapfilePath, newSwapfile);
    if (checkStreamError(oldSwapfile, "in oldSwapfile before compaction")) {
        ioError = true;
    }
    if (checkStreamError(newSwapfile, "in newSwapfile before compaction")) {
        ioError = true;
    }

    std::map<void *, std::streampos> newObjectOffsetMap;
    swapFileMapsMutex.SharedLock(self);
    for (auto it = objectOffsetMap.begin(); it != objectOffsetMap.end(); it++) {
        if (ioError == true) {
            break;
        }

        void * object = it->first;
        std::streampos oldPos = it->second;
        size_t objectSize = objectSizeMap[object];
        char * objectData = new char[objectSize];

        oldSwapfile.seekg(oldPos);
        oldSwapfile.read(objectData, objectSize);

        std::streampos newPos = newSwapfile.tellp();
        newSwapfile.write(objectData, objectSize);
        newObjectOffsetMap[object] = newPos;
        delete[] objectData;

        swapfileObjects++;
        swapfileSize += objectSize;

        if (checkStreamError(oldSwapfile, "in oldSwapfile during compaction")) {
            ioError = true;
        }
        if (checkStreamError(newSwapfile, "in newSwapfile during compaction")) {
            ioError = true;
        }
    }
    swapFileMapsMutex.SharedUnlock(self);

    oldSwapfile.close();
    newSwapfile.close();

    swapFileMapsMutex.ExclusiveLock(self);
    objectOffsetMap = newObjectOffsetMap;
    swapFileMutex.ExclusiveLock(self);
    swapfile.close();
    remove(swapfilePath.c_str());
    rename(newSwapfilePath.c_str(), swapfilePath.c_str());
    openFileAppend(swapfilePath, swapfile);
    if (checkStreamError(swapfile, "reopening swapfile after compaction")) {
        ioError = true;
    }
    swapFileMutex.ExclusiveUnlock(self);
    swapFileMapsMutex.ExclusiveUnlock(self);

    if (ioError) {
        LOG(INFO) << "NIEL detected errors while compacting swap file";
    }
    else {
        LOG(INFO) << "NIEL finished compacting swap file; new swap file has " << swapfileObjects
                  << " objects, size " << swapfileSize;
    }
}

void CheckAndUpdate(gc::collector::GarbageCollector * gc, mirror::Object * object) {
    if (!swapEnabled) {
        return;
    }

    if (gc->GetGcType() != gc::collector::kGcTypePartial) {
        return;
    }

    if (object->GetStubFlag()) {
        return;
    }

    object->SetIgnoreReadFlag();
    size_t objectSize = object->SizeOf();
    bool isSwappableType = objectIsSwappableType(object);
    object->ClearIgnoreReadFlag();

    uint8_t readCounterVal = object->GetReadCounter();
    uint8_t writeCounterVal = object->GetWriteCounter();

    bool wasRead = (readCounterVal > 0);
    bool wasWritten = (writeCounterVal > 0);

    //uint8_t rsrVal = object->GetReadShiftRegister();
    uint8_t wsrVal = object->GetWriteShiftRegister();

    gc::Heap * heap = getHeapChecked();
    bool shouldSwapPreliminary =
        objectIsLarge(objectSize)
        && objectIsCold(wsrVal, wasWritten)
        && !object->GetNoSwapFlag()
        && objectInSwappableSpace(heap, object)
        && isSwappableType
    ;

    bool shouldSwap = false;
    if (shouldSwapPreliminary) {
        object->SetIgnoreReadFlag();
        bool isExcluded = shouldExcludeObjectAndMembers(object);
        if (isExcluded) {
            object->SetNoSwapFlag();
            ExcludeVisitor visitor;
            DummyReferenceVisitor referenceVisitor;
            object->VisitReferences(visitor, referenceVisitor);
        }
        object->ClearIgnoreReadFlag();
        shouldSwap = shouldSwapPreliminary && !isExcluded;
    }

    if (shouldSwap) {
        swapOutSet.insert(object);
        if (object->GetDirtyBit()) {
            Thread * self = Thread::Current();
            writeQueueMutex.ExclusiveLock(self);
            if (!writeSet.count(object)) {
                writeSet.insert(object);
                writeQueue.push_back(object);
            }
            writeQueueMutex.ExclusiveUnlock(self);
        }
    }

    if (wasRead) {
        object->ClearReadCounter();
    }
    if (wasWritten) {
        object->ClearWriteCounter();
    }

    object->UpdateReadShiftRegister(wasRead);
    object->UpdateWriteShiftRegister(wasWritten);
}

void validateSwapFile(Thread * self) {
    bool error = false;

    LOG(INFO) << "NIEL starting swap file validation";

    swapFileMutex.ExclusiveLock(self);
    swapFileMapsMutex.SharedLock(self);

    if (objectOffsetMap.size() != objectSizeMap.size()) {
        LOG(ERROR) << "NIELERROR: objectOffsetMap and objectSizeMap sizes differ";
        error = true;
    }

    std::streampos curPos = swapfile.tellp();
    if (checkStreamError(swapfile, "validation initial tellp()")) {
        error = true;
    }

    for (auto it = objectOffsetMap.begin(); it != objectOffsetMap.end(); it++) {
        void * object = it->first;
        std::streampos offset = it->second;

        if (!objectSizeMap.count(object)) {
            LOG(ERROR) << "NIELERROR: objectSizeMap does not contain object " << object;
            error = true;
        }

        size_t objectSize = objectSizeMap[object];
        char * objectData = new char[objectSize];

        swapfile.seekg(offset);
        if (checkStreamError(swapfile, "validation seekg()")) {
            error = true;
        }
        swapfile.read(objectData, objectSize);
        if (checkStreamError(swapfile, "validation read()")) {
            error = true;
        }

        if (objectSize < 16) {
            LOG(ERROR) << "NIELERROR: object size " << objectSize << " is too small";
            error = true;
        }
    }

    swapfile.seekp(curPos);
    if (checkStreamError(swapfile, "validation final seekp()")) {
        error = true;
    }

    swapFileMapsMutex.SharedUnlock(self);
    swapFileMutex.ExclusiveUnlock(self);

    if (error) {
        LOG(ERROR) << "NIELERROR swap file validation detected errors";
    }
    else {
        LOG(INFO) << "NIEL swap file validation successful";
    }
}

void debugPrintDataStructureInfo(Thread * self, const std::string & message) {
  int objectOffsetMapStubCount = 0;
  int objectOffsetMapTotalCount = 0;
  int objectStubMapCount = 0;
  swapFileMapsMutex.SharedLock(self);
  for (auto it = objectOffsetMap.begin(); it != objectOffsetMap.end(); it++) {
    mirror::Object * obj = (mirror::Object *)it->first;
    if (obj->GetStubFlag()) {
      objectOffsetMapStubCount++;
    }
  }
  objectOffsetMapTotalCount = objectOffsetMap.size();
  swapFileMapsMutex.SharedUnlock(self);

  objectStubMapMutex.SharedLock(self);
  objectStubMapCount = objectStubMap.size();
  objectStubMapMutex.SharedUnlock(self);

  LOG(INFO) << "NIEL (" << message << ") objectOffsetMap contains "
            << objectOffsetMapTotalCount << " total objects, " << objectOffsetMapStubCount
            << " stubs; objectStubMap contains " << objectStubMapCount << " object-stub pairs";
}

class DumpObjectVisitor {
  public:
    void operator()(mirror::Object * obj,
                    MemberOffset offset,
                    bool is_static ATTRIBUTE_UNUSED) const
            SHARED_REQUIRES(Locks::mutator_lock_) {
        mirror::Object * ref = obj->GetFieldObject<mirror::Object>(offset);
        std::string refString;
        if (ref == nullptr) {
            refString = "null";
        }
        else if (ref->GetStubFlag()) {
            refString = "stub";
        }
        else if (ref->GetClass() == nullptr) {
            refString = "null class";
        }
        else {
            refString = PrettyClass(ref->GetClass());
        }
        LOG(INFO) << "ref: " << ref << " " << refString;
    }
    void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED) const {}
    void VisitRoot(mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED) const {}
};

class DumpObjectReferenceVisitor {
  public:
    void operator()(mirror::Class* klass ATTRIBUTE_UNUSED,
                    mirror::Reference* ref) const
            SHARED_REQUIRES(Locks::mutator_lock_) {
        mirror::Object * referent = ref->GetReferent();
        std::string referentString;
        if (referent == nullptr) {
            referentString = "null";
        }
        else if (referent->GetStubFlag()) {
            referentString = "stub";
        }
        else if (referent->GetClass() == nullptr) {
            referentString = "null class";
        }
        else {
            referentString = PrettyClass(referent->GetClass());
        }
        LOG(INFO) << "reference referent: " << referent << " " << referentString;
    }
};

void dumpObject(mirror::Object * obj) SHARED_REQUIRES(Locks::mutator_lock_) {
    LOG(INFO) << "NIEL dump of object @" << obj;
    LOG(INFO) << "size: " << obj->SizeOf();
    LOG(INFO) << "class: "
              << (obj->GetClass() == nullptr ? "null" : PrettyClass(obj->GetClass()));

    DumpObjectVisitor visitor;
    DumpObjectReferenceVisitor refVisitor;
    obj->VisitReferences(visitor, refVisitor);

    LOG(INFO) << "NIEL end dump of object @" << obj;
}

} // namespace swap
} // namespace niel
} // namespace art
