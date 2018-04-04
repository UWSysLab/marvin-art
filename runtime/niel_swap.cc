#include "niel_swap.h"

#include "gc/collector/garbage_collector.h"
#include "gc/heap.h"
#include "gc/task_processor.h"
#include "mirror/object.h"
#include "mirror/object-inl.h"
#include "niel_stub-inl.h"
#include "runtime.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <queue>
#include <vector>

namespace art {

namespace niel {

namespace swap {

/* Constants */
const int MAGIC_NUM = 42424242;
const double COMPACT_THRESHOLD = 0.25;

/* Internal functions */
std::string getPackageName();
void openFile(const std::string & path, std::fstream & stream);
bool checkStreamError(const std::ios & stream, const std::string & msg);
void validateSwapFile();
void replaceDataStructurePointers(const std::map<void *, void *> & addrTable);
gc::Heap * getHeapChecked();
gc::TaskProcessor * getTaskProcessorChecked();
void dumpObject(mirror::Object * obj);

/* Variables */
Mutex writeQueueMutex("WriteQueueMutex", kLoggingLock);

// Not locked but assumed to be only touched in one thread (because all Tasks,
// including GC, run on the same fixed thread)
std::fstream swapfile;
uint32_t pid = 0;
std::map<void *, std::streampos> objectOffsetMap;
std::map<void *, size_t> objectSizeMap;
std::map<void *, void *> remappingTable;
std::map<void *, void *> semiSpaceRemappingTable; //TODO: combine with normal remapping table?
std::queue<mirror::Object *> swapOutQueue;
int freedObjects = 0;
long freedSize = 0;
int swapfileObjects = 0;
int swapfileSize = 0;

// Locked by writeQueueMutex
std::vector<mirror::Object *> writeQueue;
std::set<mirror::Object *> writeSet; // prevent duplicate entries in writeQueue

class WriteTask : public gc::HeapTask {
  public:
    WriteTask(uint64_t target_time) : gc::HeapTask(target_time) { }

    virtual void Run(Thread * self) {
        bool done = false;
        uint64_t startTime = NanoTime();
        bool ioError = false;

        int numGarbageObjects = 0;
        int numNullClasses = 0;
        int numSpacelessObjects = 0;
        int numResizedObjects = 0;

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

            bool objectInSpace = false;
            gc::Heap * heap = Runtime::Current()->GetHeap();
            for (auto & space : heap->GetContinuousSpaces()) {
                if (space->Contains(object)) {
                    objectInSpace = true;
                }
            }
            for (auto & space : heap->GetDiscontinuousSpaces()) {
                if (space->Contains(object)) {
                    objectInSpace = true;
                }
            }
            if (!objectInSpace) {
                numSpacelessObjects++;
            }

            bool validObject = false;
            if (object != nullptr && objectInSpace) {
                object->SetIgnoreReadFlag();
                if (object->GetPadding() == MAGIC_NUM) {
                    if (object->GetClass() != nullptr) {
                        validObject = true;
                    }
                    else {
                        numNullClasses++;
                    }
                }
                else {
                    numGarbageObjects++;
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

                if (objectOffsetMap.find(object) == objectOffsetMap.end()) {
                    std::streampos offset = swapfile.tellp();
                    swapfile.write(objectData, objectSize);
                    bool writeError = checkStreamError(swapfile,
                            "writing object to swapfile for the first time in WriteTask");
                    if (writeError) {
                        ioError = true;
                    }
                    objectOffsetMap[object] = offset;
                    objectSizeMap[object] = objectSize;

                    swapfileObjects++;
                    swapfileSize += objectSize;
                }
                else {
                    std::streampos offset = objectOffsetMap[object];
                    size_t size = objectSizeMap[object];
                    if (size != objectSize) {
                        numResizedObjects++;
                    }
                    else {
                        std::streampos curpos = swapfile.tellp();
                        swapfile.seekp(offset);
                        swapfile.write(objectData, objectSize);
                        bool writeError = checkStreamError(swapfile,
                                "writing object to swapfile again in WriteTask");
                        if (writeError) {
                            ioError = true;
                        }
                        swapfile.seekp(curpos);
                    }
                }

                delete[] objectData;

                if (ioError) {
                    done = true;
                }

                uint64_t currentTime = NanoTime();
                if (currentTime - startTime > 300000000) {
                    done = true;
                }
            }
        }
        Locks::mutator_lock_->ReaderUnlock(self);

        LOG(INFO) << "NIEL done writing objects in WriteTask; " << queueSize
                  << " objects still in queue; swap file has " << swapfileObjects
                  << " objects, size " << swapfileSize;
        if (numGarbageObjects > 0 || numNullClasses > 0 || numSpacelessObjects > 0
                || numResizedObjects > 0) {
            LOG(INFO) << "NIEL WriteTask irregularities: " << numGarbageObjects
                      << " garbage objects, " << numNullClasses << " null classes, "
                      << numSpacelessObjects << " objects not in any space, "
                      << numResizedObjects << " resized objects";
        }

        if ((double)freedSize / swapfileSize > COMPACT_THRESHOLD) {
            CompactSwapFile();
            validateSwapFile();
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
//
// TODO: Figure out if this class should actually be doing something
class PatchDummyReferenceVisitor {
  public:
    void operator()(mirror::Class* klass ATTRIBUTE_UNUSED,
                    mirror::Reference* ref ATTRIBUTE_UNUSED) const {}
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
        for (int i = 0; i < stub->GetNumRefs(); i++) {
            mirror::Object * ref = stub->GetReference(i);
            if (remappingTable.count(ref)) {
                stub->SetReference(i, (mirror::Object *)remappingTable[ref]);
            }
        }
    }
    else {
        PatchVisitor visitor;
        PatchDummyReferenceVisitor dummyVisitor;
        obj->VisitReferences(visitor, dummyVisitor);
    }
}

void PatchStubReferences(Thread * self ATTRIBUTE_UNUSED, gc::Heap * heap) {
    LOG(INFO) << "NIEL start patching stub references";
    uint64_t startTime = NanoTime();

    gc::space::LargeObjectSpace * largeObjectSpace = heap->GetLargeObjectsSpace();
    largeObjectSpace->Walk(&PatchCallback, nullptr);

    gc::space::RosAllocSpace * rosAllocSpace = heap->GetRosAllocSpace();
    rosAllocSpace->Walk(&PatchCallback, nullptr);

    replaceDataStructurePointers(remappingTable);

    remappingTable.clear();

    uint64_t endTime = NanoTime();
    uint64_t duration = endTime - startTime;
    LOG(INFO) << "NIEL patching stub references took " << PrettyDuration(duration);
}

void replaceDataStructurePointers(const std::map<void *, void *> & addrTable) {
    for (auto it = addrTable.begin(); it != addrTable.end(); it++) {
        void * originalPtr = it->first;
        void * newPtr = it->second;

        auto oomIt = objectOffsetMap.find(originalPtr);
        objectOffsetMap[newPtr] = oomIt->second;
        objectOffsetMap.erase(oomIt);

        auto osmIt = objectSizeMap.find(originalPtr);
        objectSizeMap[newPtr] = osmIt->second;
        objectSizeMap.erase(osmIt);
    }
}

void ReplaceObjectsWithStubs(Thread * self, gc::Heap * heap) {
    while (!swapOutQueue.empty()) {
        mirror::Object * obj = swapOutQueue.front();
        if (obj->GetDirtyBit()) {
            return;
        }

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
        Stub * stub = (Stub *)stubData;
        stub->PopulateFrom(obj);

        if (heap->GetLargeObjectsSpace()->Contains(obj)) {
            stub->SetLargeObjectFlag();
        }

        heap->GetRosAllocSpace()->Free(self, obj);

        remappingTable[obj] = stub;
        swapOutQueue.pop();
    }
}

void RecordForwardedObject(mirror::Object * obj, mirror::Object * forwardAddress) {
    if (objectOffsetMap.count(obj)) {
        semiSpaceRemappingTable[obj] = forwardAddress;
    }
}

void SemiSpaceUpdateDataStructures() {
    replaceDataStructurePointers(semiSpaceRemappingTable);
    semiSpaceRemappingTable.clear();
}

void SwapObjectsIn(gc::Heap * heap) {
    LOG(INFO) << "Start swapping objects back in";
    uint64_t startTime = NanoTime();

    Thread * self = Thread::Current();
    for (auto it = objectOffsetMap.begin(); it != objectOffsetMap.end(); it++) {
        mirror::Object * obj = (mirror::Object *)it->first;
        if (obj->GetStubFlag()) {
            Stub * stub = (Stub *)obj;
            size_t objSize = objectSizeMap[stub];

            mirror::Object * newObj = nullptr;
            if (stub->GetLargeObjectFlag()) {
                //TODO: allocate in large object space
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

            if (newObj == nullptr) {
                LOG(INFO) << "NIELERROR failed to allocate object of size " << objSize
                          << "; dumping stub";
                stub->SemanticDump();
            }
            else {
                std::streampos objOffset = objectOffsetMap[stub];
                std::streampos curPos = swapfile.tellp();
                swapfile.seekg(objOffset);
                swapfile.read((char *)newObj, objSize);
                swapfile.seekg(curPos);

                stub->CopyRefsInto(newObj);
                dumpObject(newObj);

                heap->GetRosAllocSpace()->Free(self, obj);

                remappingTable[stub] = newObj;
            }
        }
    }
    PatchStubReferences(self, heap);
    replaceDataStructurePointers(remappingTable);

    uint64_t endTime = NanoTime();
    uint64_t duration = endTime - startTime;
    LOG(INFO) << "NIEL swapping objects back in took " << PrettyDuration(duration);
}

gc::Heap * getHeapChecked() {
    Runtime * runtime = Runtime::Current();
    if (runtime == nullptr) {
        return nullptr;
    }
    return runtime->GetHeap();
}

gc::TaskProcessor * getTaskProcessorChecked() {
    gc::Heap * heap = getHeapChecked();
    if (heap == nullptr) {
        return nullptr;
    }
    return heap->GetTaskProcessor();
}

void GcRecordFree(Thread * self, mirror::Object * object) {
    writeQueueMutex.ExclusiveLock(self);
    auto writeQueuePos = std::find(writeQueue.begin(), writeQueue.end(), object);
    if (writeQueuePos != writeQueue.end()) {
        writeQueue.erase(writeQueuePos);
        writeSet.erase(object);
    }
    writeQueueMutex.ExclusiveUnlock(self);

    if (objectOffsetMap.count(object) && objectSizeMap.count(object)) {
        freedObjects++;
        freedSize += objectSizeMap[object];
        objectOffsetMap.erase(object);
        objectSizeMap.erase(object);
    }
    else if (objectOffsetMap.count(object) || objectSizeMap.count(object)) {
        LOG(INFO) << "NIEL ERROR: object in one of the object maps but not the other";
    }
}

void InitIfNecessary() {
    uint32_t curPid = getpid();
    if (curPid == pid) {
        return;
    }

    gc::TaskProcessor * taskProcessor = getTaskProcessorChecked();
    if (taskProcessor == nullptr || !taskProcessor->IsRunning()) {
        LOG(ERROR) << "NIEL failed to init swap since heap's TaskProcessor is null or not ready"
                   << " (or heap is null)";
        return;
    }

    // Once we reach this point, we will not try to init swap again
    // until the next time the PID changes

    pid = curPid;
    objectOffsetMap.clear();
    objectSizeMap.clear();
    writeQueue.clear();
    writeSet.clear();

    std::string packageName = getPackageName();
    std::string swapfilePath("/data/data/" + packageName + "/swapfile");
    openFile(swapfilePath, swapfile);
    swapfile.write((char *)&pid, 4);
    swapfile.flush();

    bool ioError = checkStreamError(swapfile, "after opening swapfile");
    if (ioError) {
        LOG(ERROR) << "NIEL not scheduling first WriteTask due to IO error (package name "
                  << packageName << ")";
        return;
    }

    taskProcessor->AddTask(Thread::Current(), new WriteTask(NanoTime()));

    LOG(INFO) << "NIEL successfully initialized swap for package " << packageName;
}

void CompactSwapFile() {
    LOG(INFO) << "NIEL compacting swap file";

    std::string swapfilePath("/data/data/" + getPackageName() + "/swapfile");
    std::string oldSwapfilePath("/data/data/" + getPackageName() + "/oldswapfile");
    bool ioError = false;

    freedObjects = 0;
    freedSize = 0;
    swapfileObjects = 0;
    swapfileSize = 0;

    swapfile.close();
    rename(swapfilePath.c_str(), oldSwapfilePath.c_str());

    std::fstream oldSwapfile;
    oldSwapfile.open(oldSwapfilePath, std::ios::binary | std::ios::in);

    openFile(swapfilePath, swapfile);

    std::map<void *, std::streampos> newObjectOffsetMap;

    if (checkStreamError(oldSwapfile, "in oldSwapfile before compaction")) {
        ioError = true;
    }
    if (checkStreamError(swapfile, "in swapfile before compaction")) {
        ioError = true;
    }

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

        std::streampos newPos = swapfile.tellp();
        swapfile.write(objectData, objectSize);
        newObjectOffsetMap[object] = newPos;
        delete[] objectData;

        swapfileObjects++;
        swapfileSize += objectSize;

        if (checkStreamError(oldSwapfile, "in oldSwapfile during compaction")) {
            ioError = true;
        }
        if (checkStreamError(swapfile, "in swapfile during compaction")) {
            ioError = true;
        }
    }

    objectOffsetMap = newObjectOffsetMap;

    oldSwapfile.close();

    if (ioError) {
        LOG(INFO) << "NIEL detected errors while compacting swap file";
    }
    else {
        LOG(INFO) << "NIEL finished compacting swap file; new swap file has " << swapfileObjects
                  << " objects, size " << swapfileSize;
    }
}

void CheckAndUpdate(gc::collector::GarbageCollector * gc, mirror::Object * object) {
    if (!strstr(gc->GetName(), "partial concurrent mark sweep")) {
        return;
    }

    if (object->GetStubFlag()) {
        return;
    }

    object->SetIgnoreReadFlag();
    size_t objectSize = object->SizeOf();
    object->ClearIgnoreReadFlag();

    uint8_t readCounterVal = object->GetReadCounter();
    uint8_t writeCounterVal = object->GetWriteCounter();

    bool wasRead = (readCounterVal > 0);
    bool wasWritten = (writeCounterVal > 0);

    //uint8_t rsrVal = object->GetReadShiftRegister();
    uint8_t wsrVal = object->GetWriteShiftRegister();

    if (objectSize > 200 && wsrVal < 2 && !wasWritten && object->GetDirtyBit()) {
        Thread * self = Thread::Current();
        writeQueueMutex.ExclusiveLock(self);
        if (!writeSet.count(object)) {
            writeSet.insert(object);
            writeQueue.push_back(object);
            object->SetPadding(MAGIC_NUM);
        }
        writeQueueMutex.ExclusiveUnlock(self);
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

void validateSwapFile() {
    bool error = false;

    LOG(INFO) << "NIEL starting swap file validation";

    if (objectOffsetMap.size() != objectSizeMap.size()) {
        LOG(INFO) << "NIEL ERROR: objectOffsetMap and objectSizeMap sizes differ";
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
            LOG(INFO) << "NIEL ERROR: objectSizeMap does not contain object " << object;
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
            LOG(INFO) << "NIEL ERROR: object size " << objectSize << " is too small";
            error = true;
        }

        int * padding = (int *)&objectData[12];
        if (*padding != MAGIC_NUM) {
            LOG(INFO) << "NIEL ERROR: object padding does not match magic num";
            error = true;
        }
    }

    swapfile.seekp(curPos);
    if (checkStreamError(swapfile, "validation final seekp()")) {
        error = true;
    }

    if (error) {
        LOG(INFO) << "NIEL swap file validation detected errors";
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

bool checkStreamError(const std::ios & stream, const std::string & msg) {
    bool error = !stream;
    if (error) {
        LOG(ERROR) << "NIEL stream error: " << msg << " (" << stream.good() << " "
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
                    mirror::Reference* ref ATTRIBUTE_UNUSED) const {}
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
