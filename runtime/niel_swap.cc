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
const int MAGIC_NUM = 42424242;
const double COMPACT_THRESHOLD = 0.25;

// Return values of writeToSwapFile().
const int SWAPFILE_WRITE_OK = 0;
const int SWAPFILE_WRITE_GARBAGE = 1;
const int SWAPFILE_WRITE_NULL_CLASS = 2;
const int SWAPFILE_WRITE_RESIZED = 3;

/* Internal functions */
std::string getPackageName();
void openFile(const std::string & path, std::fstream & stream);
void openFileAppend(const std::string & path, std::fstream & stream);
bool checkStreamError(const std::ios & stream, const std::string & msg);
bool isAppBlacklisted();
gc::TaskProcessor * getTaskProcessorChecked();
void FreeFromRosAllocSpace(Thread * self, gc::Heap * heap, mirror::Object * obj)
        SHARED_REQUIRES(Locks::mutator_lock_);
void FreeFromLargeObjectSpace(Thread * self, gc::Heap * heap, mirror::Object * obj)
        SHARED_REQUIRES(Locks::mutator_lock_);
void dumpObject(mirror::Object * obj);

/*
 * Writes a snapshot of the given object to the swap file, overwriting the object's
 * previous snapshot in the swap file if one exists. Clears the object's dirty bit.
 * Grabs the swapFileMutex and swapStateMutex during execution.
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
 * For each key-value pair (a,b) in addrTable, replaces any key-value pair
 * (a,c) in objectOffsetMap or objectSizeMap with the new entry (b,c).
 */
void replaceDataStructurePointers(Thread * self, const std::map<void *, void *> & addrTable);

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

/* Variables */
Mutex writeQueueMutex("WriteQueueMutex", kLoggingLock);
Mutex swappedInSetMutex("SwappedInSetMutex", kLoggingLock);
ReaderWriterMutex swapStateMutex("SwapStateMutex", kDefaultMutexLevel);
// Holding the swapFileMutex while reading the swap file always guarantees that
// reads return valid data, and it guarantees that reads reflect the latest
// writes iff the WriteTask always runs on the same thread as swap file
// compaction (as it does for marlin builds running on a Pixel XL).
Mutex swapFileMutex("SwapFileMutex", kLoggingLock);

// Not locked but assumed to be only touched in one thread (because all Tasks,
// including GC, run on the same fixed thread)
uint32_t pid = 0;
std::map<void *, void *> remappingTable;
std::map<void *, void *> semiSpaceRemappingTable; //TODO: combine with normal remapping table?
std::set<mirror::Object *> swapOutSet;
bool swappingOutObjects = false;
bool doingSwapInCleanup = false;
int freedObjects = 0;
long freedSize = 0;
int swapfileObjects = 0;
int swapfileSize = 0;

// Locked by swapFileMutex
std::fstream swapfile;

// Locked by swapStateMutex
std::map<void *, std::streampos> objectOffsetMap;
std::map<void *, size_t> objectSizeMap;

// Locked by writeQueueMutex
std::vector<mirror::Object *> writeQueue;
std::set<mirror::Object *> writeSet; // prevents duplicate entries in writeQueue

// Locked by swappedInSetMutex
std::set<Stub *> swappedInSet;

class WriteTask : public gc::HeapTask {
  public:
    WriteTask(uint64_t target_time) : gc::HeapTask(target_time) { }

    virtual void Run(Thread * self) {
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

            if (object != nullptr) {
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
            if (currentTime - startTime > 300000000) {
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

        uint64_t nanoTime = NanoTime();
        uint64_t targetTime = nanoTime + 3000000000;
        gc::Heap * curHeap = getHeapChecked();
        gc::TaskProcessor * taskProcessor = getTaskProcessorChecked();
        if (taskProcessor != nullptr && taskProcessor->IsRunning()) {
            if (ioError) {
                LOG(INFO) << "NIEL not scheduling WriteTask again due to IO error";
            }
            else {
                curHeap->RequestConcurrentGC(self, true);
                taskProcessor->AddTask(self, new WriteTask(targetTime));
            }
        }
    }
};

int writeToSwapFile(Thread * self, gc::Heap * heap, mirror::Object * object, bool * ioError) {
    int result = SWAPFILE_WRITE_OK;
    *ioError = false;

    CHECK(   heap->GetRosAllocSpace()->Contains(object)
          || heap->GetLargeObjectsSpace()->Contains(object))
        << " addr " << object;

    bool validObject = false;
    if (object != nullptr) {
        object->SetIgnoreReadFlag();
        if (object->GetPadding() == MAGIC_NUM) {
            if (object->GetClass() != nullptr) {
                validObject = true;
            }
            else {
                result = SWAPFILE_WRITE_NULL_CLASS;
            }
        }
        else {
            result = SWAPFILE_WRITE_GARBAGE;
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

        swapStateMutex.SharedLock(self);
        bool inSwapFile = (objectOffsetMap.find(object) != objectOffsetMap.end());
        swapStateMutex.SharedUnlock(self);

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

            swapStateMutex.ExclusiveLock(self);
            objectOffsetMap[object] = offset;
            objectSizeMap[object] = objectSize;
            swapStateMutex.ExclusiveUnlock(self);

            swapfileObjects++;
            swapfileSize += objectSize;
        }
        else {
            swapStateMutex.SharedLock(self);
            std::streampos offset = objectOffsetMap[object];
            size_t size = objectSizeMap[object];
            swapStateMutex.SharedUnlock(self);

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

void patchStubReferences(Thread * self, gc::Heap * heap) {
    ScopedTimer timer("patching stub references");

    gc::space::LargeObjectSpace * largeObjectSpace = heap->GetLargeObjectsSpace();
    largeObjectSpace->Walk(&PatchCallback, nullptr);

    gc::space::RosAllocSpace * rosAllocSpace = heap->GetRosAllocSpace();
    rosAllocSpace->Walk(&PatchCallback, nullptr);

    replaceDataStructurePointers(self, remappingTable);
}

//TODO: figure out whether it's better to grab the lock once or keep grabbing
//      and releasing
void replaceDataStructurePointers(Thread * self, const std::map<void *, void *> & addrTable) {
    swapStateMutex.ExclusiveLock(self);
    for (auto it = addrTable.begin(); it != addrTable.end(); it++) {
        void * originalPtr = it->first;
        void * newPtr = it->second;

        CHECK_NE(objectOffsetMap.count(originalPtr), 0u);
        CHECK_NE(objectSizeMap.count(originalPtr), 0u);

        auto oomIt = objectOffsetMap.find(originalPtr);
        objectOffsetMap[newPtr] = oomIt->second;
        objectOffsetMap.erase(oomIt);

        auto osmIt = objectSizeMap.find(originalPtr);
        objectSizeMap[newPtr] = osmIt->second;
        objectSizeMap.erase(osmIt);
    }
    swapStateMutex.ExclusiveUnlock(self);
}

void FreeFromRosAllocSpace(Thread * self, gc::Heap * heap, mirror::Object * obj) {
    heap->GetRosAllocSpace()->GetLiveBitmap()->Clear(obj);
    heap->GetRosAllocSpace()->Free(self, obj);
}

void FreeFromLargeObjectSpace(Thread * self, gc::Heap * heap, mirror::Object * obj) {
    heap->GetLargeObjectsSpace()->GetLiveBitmap()->Clear(obj);
    heap->GetLargeObjectsSpace()->Free(self, obj);
}

void SwapObjectsOut(Thread * self, gc::Heap * heap) {
    // First make sure all objects in swapOutSet have been written to the swap file
    for (auto it = swapOutSet.begin(); it != swapOutSet.end(); it++) {
        mirror::Object * obj = *it;
        if (obj->GetDirtyBit()) {
            obj->SetPadding(MAGIC_NUM);
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

        swapStateMutex.SharedLock(self);
        CHECK(objectOffsetMap.count(obj) && objectSizeMap.count(obj));
        swapStateMutex.SharedUnlock(self);

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

        Stub * stub = (Stub *)stubData;
        stub->PopulateFrom(obj);

        stub->SetObjectAddress(nullptr);

        if (heap->GetLargeObjectsSpace()->Contains(obj)) {
            stub->SetLargeObjectFlag();
            FreeFromLargeObjectSpace(self, heap, obj);
        }
        else {
            FreeFromRosAllocSpace(self, heap, obj);
        }

        remappingTable[obj] = stub;
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
    swapStateMutex.SharedLock(self);
    if (objectOffsetMap.count(obj)) {
        semiSpaceRemappingTable[obj] = forwardAddress;
    }
    swapStateMutex.SharedUnlock(self);
}

void SemiSpaceUpdateDataStructures(Thread * self) {
    replaceDataStructurePointers(self, semiSpaceRemappingTable);
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

    CHECK(newObj->GetPadding() == MAGIC_NUM);
    CHECK(newObj->GetDirtyBit() == 0);

    stub->CopyRefsInto(newObj);
    stub->SetObjectAddress(newObj);

    return newObj;
}

//TODO: make sure obj doesn't get freed during GC as long as stub isn't freed,
//      but is freed when stub is freed
void SwapInOnDemand(Stub * stub) {
    gc::Heap * heap = getHeapChecked();
    CHECK(heap->GetRosAllocSpace()->Contains((mirror::Object *)stub));

    Thread * self = Thread::Current();

    swapStateMutex.SharedLock(self);
    std::streampos objOffset = objectOffsetMap[stub];
    size_t objSize = objectSizeMap[stub];
    swapStateMutex.SharedUnlock(self);

    swapInObject(self, heap, stub, objOffset, objSize);

    swappedInSetMutex.ExclusiveLock(self);
    swappedInSet.insert(stub);
    swappedInSetMutex.ExclusiveUnlock(self);
}

void CleanUpOnDemandSwaps(gc::Heap * heap) REQUIRES(Locks::mutator_lock_) {
    doingSwapInCleanup = true;
    Thread * self = Thread::Current();

    swappedInSetMutex.ExclusiveLock(self);
    for (auto it = swappedInSet.begin(); it != swappedInSet.end(); it++) {
        Stub * stub = *it;
        CHECK(heap->GetRosAllocSpace()->Contains((mirror::Object *)stub));

        remappingTable[stub] = stub->GetObjectAddress();
        FreeFromRosAllocSpace(self, heap, (mirror::Object *)stub);
    }
    swappedInSet.clear();
    swappedInSetMutex.ExclusiveUnlock(self);

    patchStubReferences(self, heap);
    remappingTable.clear();
    doingSwapInCleanup = false;
}

void SwapObjectsIn(gc::Heap * heap) {
    ScopedTimer timer("swapping objects back in");
    CHECK_EQ(remappingTable.size(), 0u);

    doingSwapInCleanup = true;
    Thread * self = Thread::Current();
    swapStateMutex.SharedLock(self);
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
            FreeFromRosAllocSpace(self, heap, obj);
            remappingTable[stub] = swappedInObj;
        }
    }
    swapStateMutex.SharedUnlock(self);
    patchStubReferences(self, heap);
    remappingTable.clear();
    doingSwapInCleanup = false;
}

gc::TaskProcessor * getTaskProcessorChecked() {
    gc::Heap * heap = getHeapChecked();
    if (heap == nullptr) {
        return nullptr;
    }
    return heap->GetTaskProcessor();
}

void GcRecordFree(Thread * self, mirror::Object * object) {
    /*
     * The correctness of this check depends on upon several assumptions:
     * 1) Only the garbage collectors and my swapping code call Free() on the
     *    RosAlloc space and large object space.
     * 2) The garbage collectors never run concurrently with any swapping
     *    methods that free objects or stubs as part of swapping (currently,
     *    these methods are SwapObjectsOut(), SwapObjectsIn(), and
     *    CleanUpOnDemandSwaps()).
     */
    if (swappingOutObjects || doingSwapInCleanup) {
        return;
    }

    writeQueueMutex.ExclusiveLock(self);
    auto writeQueuePos = std::find(writeQueue.begin(), writeQueue.end(), object);
    if (writeQueuePos != writeQueue.end()) {
        writeQueue.erase(writeQueuePos);
        writeSet.erase(object);
    }
    writeQueueMutex.ExclusiveUnlock(self);

    swapStateMutex.ExclusiveLock(self);
    if (objectOffsetMap.count(object) && objectSizeMap.count(object)) {
        freedObjects++;
        freedSize += objectSizeMap[object];
        objectOffsetMap.erase(object);
        objectSizeMap.erase(object);
    }
    else if (objectOffsetMap.count(object) || objectSizeMap.count(object)) {
        LOG(ERROR) << "NIELERROR: object in one of the object maps but not the other";
    }
    swapStateMutex.ExclusiveUnlock(self);

    if (swapOutSet.count(object)) {
        swapOutSet.erase(object);
    }
}

bool isAppBlacklisted(const std::string & packageName) {
    std::set<std::string> blacklist;
    blacklist.insert("droid.launcher3");

    for (auto it = blacklist.begin(); it != blacklist.end(); it++) {
        if (packageName.find(*it) != std::string::npos) {
            return true;
        }
    }
    return false;
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

    swapStateMutex.ExclusiveLock(self);
    objectOffsetMap.clear();
    objectSizeMap.clear();
    swapStateMutex.ExclusiveUnlock(self);

    writeQueueMutex.ExclusiveLock(self);
    writeQueue.clear();
    writeSet.clear();
    writeQueueMutex.ExclusiveUnlock(self);

    std::string packageName = getPackageName();
    std::string swapfilePath("/data/data/" + packageName + "/swapfile");

    if (isAppBlacklisted(packageName)) {
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
    swapStateMutex.SharedLock(self);
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
    swapStateMutex.SharedUnlock(self);

    oldSwapfile.close();
    newSwapfile.close();

    swapStateMutex.ExclusiveLock(self);
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
    swapStateMutex.ExclusiveUnlock(self);

    if (ioError) {
        LOG(INFO) << "NIEL detected errors while compacting swap file";
    }
    else {
        LOG(INFO) << "NIEL finished compacting swap file; new swap file has " << swapfileObjects
                  << " objects, size " << swapfileSize;
    }
}

void CheckAndUpdate(gc::collector::GarbageCollector * gc, mirror::Object * object) {
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
    bool shouldSwap =
        objectIsLarge(objectSize)
        && objectIsCold(wsrVal, wasWritten)
        && !object->GetNoSwapFlag()
        && objectInSwappableSpace(heap, object)
        && isSwappableType
    ;

    if (shouldSwap) {
        swapOutSet.insert(object);
        if (object->GetDirtyBit()) {
            Thread * self = Thread::Current();
            writeQueueMutex.ExclusiveLock(self);
            if (!writeSet.count(object)) {
                writeSet.insert(object);
                writeQueue.push_back(object);
                object->SetPadding(MAGIC_NUM);
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
    swapStateMutex.SharedLock(self);

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

        int * padding = (int *)&objectData[12];
        if (*padding != MAGIC_NUM) {
            LOG(ERROR) << "NIELERROR: object padding does not match magic num";
            error = true;
        }
    }

    swapfile.seekp(curPos);
    if (checkStreamError(swapfile, "validation final seekp()")) {
        error = true;
    }

    swapStateMutex.SharedUnlock(self);
    swapFileMutex.ExclusiveUnlock(self);

    if (error) {
        LOG(ERROR) << "NIELERROR swap file validation detected errors";
    }
    else {
        LOG(INFO) << "NIEL swap file validation successful";
    }
}

std::string getPackageName() {
    std::ifstream cmdlineFile("/proc/self/cmdline");
    std::string cmdline;
    getline(cmdlineFile, cmdline);
    cmdlineFile.close();
    return cmdline.substr(0, cmdline.find((char)0));
}

void openFile(const std::string & path, std::fstream & stream) {
    stream.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!stream) {
        stream.close();
        stream.open(path, std::ios::binary | std::ios::out);
        stream.close();
        stream.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    }
}

void openFileAppend(const std::string & path, std::fstream & stream) {
    stream.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::ate);
}

bool checkStreamError(const std::ios & stream, const std::string & msg) {
    bool error = !stream;
    if (error) {
        LOG(ERROR) << "NIELERROR stream error: " << msg << " (" << stream.good() << " "
                   << stream.eof() << " " << stream.fail() << " " << stream.bad() << ")";
    }
    return error;
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
