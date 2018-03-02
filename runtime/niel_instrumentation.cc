#include "niel_instrumentation.h"

#include "gc/allocator/rosalloc.h"
#include "gc/collector/garbage_collector.h"
#include "gc/heap.h"
#include "gc/task_processor.h"
#include "gc/space/rosalloc_space.h"
#include "gc/space/space.h"
#include "globals.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/object.h"
#include "mirror/object-inl.h"
#include "thread.h"

#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <vector>

#include "niel_bivariate_histogram.h"
#include "niel_histogram.h"

namespace art {

namespace nielinst {

/* Constants */
const int LOG_INTERVAL_SECONDS = 10;
const int MAGIC_NUM = 42424242;

/* Internal functions */
void maybePrintLog();
void printAllocCounts();
void printHeap();

std::string getPackageName();
void openFile(const std::string & path, std::fstream & stream);
bool checkStreamError(const std::ios & stream, const std::string & msg);

/* Variables */
Mutex instMutex("NielInstrumentationMutex", kLoggingLock);
gc::Heap * heap = NULL;
time_t lastLogTime;

/*     Locked with instMutex */
long numTotalRosAllocThreadLocalAllocs = 0;
long numTotalRosAllocNormalAllocs = 0;
long numTotalRosAllocLargeObjectAllocs = 0;

long sizeTotalRosAllocThreadLocalAllocs = 0;
long sizeTotalRosAllocNormalAllocs = 0;
long sizeTotalRosAllocLargeObjectAllocs = 0;

long numCurrentRosAllocAllocs = 0;
long numCurrentRosAllocLargeObjectAllocs = 0;

long sizeCurrentRosAllocAllocs = 0;
long sizeCurrentRosAllocLargeObjectAllocs = 0;

std::map<std::string, int> currentAllocCounts;
std::map<std::string, int> currentAllocSizes;

std::map<std::string, int> totalAllocCounts;
std::map<std::string, int> totalAllocSizes;
/*     End locked with instMutex */

bool doingAccessCount = false;

long objectsRead = 0;
long objectsWritten = 0;
long objectsReadAndWritten = 0;
long totalObjects = 0;
long errorCount = 0;
long shenanigansCount = 0; // used for misc debugging

long readTotalObjectSize = 0;
long readTotalPointerSize = 0;
long unreadTotalObjectSize = 0;
long unreadTotalPointerSize = 0;
long smallObjectTotalObjectSize = 0;
long smallObjectTotalPointerSize = 0;
long largeObjectTotalObjectSize = 0;
long largeObjectTotalPointerSize = 0;

Histogram smallObjectPointerFracHist(10, 0, 1); // objects <=200 bytes
Histogram largeObjectPointerFracHist(10, 0, 1); // objects >200 bytes
Histogram readCountHist(10, 1, 8191);
Histogram writeCountHist(10, 1, 1023);
Histogram readShiftRegHist(16, 0, 16);
Histogram writeShiftRegHist(16, 0, 16);

BivariateHistogram readsVsPointerFracHist(10, 1, 2400, 10, 0, 1);
BivariateHistogram writesVsPointerFracHist(10, 1, 100, 10, 0, 1);
BivariateHistogram readShiftRegVsPointerFracHist(16, 0, 16, 10, 0, 1);
BivariateHistogram writeShiftRegVsPointerFracHist(16, 0, 16, 10, 0, 1);
BivariateHistogram objectSizeVsPointerFracHist(10, 0, 1000, 10, 0, 1);

std::fstream swapfile;
uint32_t pid = 0;
std::map<void *, std::streampos> objectOffsetMap;
std::map<void *, size_t> objectSizeMap;
std::vector<mirror::Object *> writeQueue;
Mutex writeQueueMutex("WriteQueueMutex", kLoggingLock);

void RecordRosAllocAlloc(Thread * self, size_t size, RosAllocAllocType type) {
    instMutex.ExclusiveLock(self);

    switch(type) {
      case ROSALLOC_ALLOC_THREAD_LOCAL:
        numTotalRosAllocThreadLocalAllocs++;
        sizeTotalRosAllocThreadLocalAllocs += size;
        numCurrentRosAllocAllocs++;
        sizeCurrentRosAllocAllocs += size;
        break;
      case ROSALLOC_ALLOC_NORMAL:
        numTotalRosAllocNormalAllocs++;
        sizeTotalRosAllocNormalAllocs += size;
        numCurrentRosAllocAllocs++;
        sizeCurrentRosAllocAllocs += size;
        break;
      case ROSALLOC_ALLOC_LARGE:
        numTotalRosAllocLargeObjectAllocs++;
        sizeTotalRosAllocLargeObjectAllocs += size;
        numCurrentRosAllocLargeObjectAllocs++;
        sizeCurrentRosAllocLargeObjectAllocs += size;
        break;
    }

    maybePrintLog();
    instMutex.ExclusiveUnlock(self);
}

void RecordRosAllocFree(Thread * self, size_t size, RosAllocFreeType type) {
    instMutex.ExclusiveLock(self);

    switch(type) {
      case ROSALLOC_FREE_NORMAL_OR_THREAD_LOCAL:
        numCurrentRosAllocAllocs--;
        sizeCurrentRosAllocAllocs -= size;
        break;
      case ROSALLOC_FREE_LARGE:
        numCurrentRosAllocLargeObjectAllocs--;
        sizeCurrentRosAllocLargeObjectAllocs -= size;
        break;
    }

    maybePrintLog();
    instMutex.ExclusiveUnlock(self);
}

void RecordAlloc(Thread * self, gc::space::Space * space, size_t size) {
    instMutex.ExclusiveLock(self);

    std::string name(space->GetName());
    currentAllocCounts[name]++;
    currentAllocSizes[name] += size;
    totalAllocCounts[name]++;
    totalAllocSizes[name] += size;

    maybePrintLog();
    instMutex.ExclusiveUnlock(self);
}

void RecordFree(Thread * self, gc::space::Space * space, size_t size, int count) {
    instMutex.ExclusiveLock(self);

    std::string name(space->GetName());
    currentAllocCounts[name] -= count;
    currentAllocSizes[name] -= size;

    maybePrintLog();
    instMutex.ExclusiveUnlock(self);
}

void SetHeap(gc::Heap * inHeap) {
    heap = inHeap;
}

void GcRecordFree(Thread * self, mirror::Object * object) {
    writeQueueMutex.ExclusiveLock(self);

    auto writeQueuePos = std::find(writeQueue.begin(), writeQueue.end(), object);
    if (writeQueuePos != writeQueue.end()) {
        writeQueue.erase(writeQueuePos);
    }

    writeQueueMutex.ExclusiveUnlock(self);
}

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

        Locks::mutator_lock_->ReaderLock(self);
        while (!done) {
            mirror::Object * object = nullptr;

            writeQueueMutex.ExclusiveLock(self);
            if (writeQueue.size() == 0) {
                done = true;
            }
            else {
                object = writeQueue.front();
                writeQueue.erase(writeQueue.begin());
            }
            writeQueueMutex.ExclusiveUnlock(self);

            bool objectInSpace = false;
            gc::Heap * tempHeap = Runtime::Current()->GetHeap();
            for (auto & space : tempHeap->GetContinuousSpaces()) {
                if (space->Contains(object)) {
                    objectInSpace = true;
                }
            }
            for (auto & space : tempHeap->GetDiscontinuousSpaces()) {
                if (space->Contains(object)) {
                    objectInSpace = true;
                }
            }
            if (!objectInSpace) {
                numSpacelessObjects++;
            }

            bool validObject = false;
            if (object != nullptr && objectInSpace) {
                object->SetIgnoreAccessFlag();
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
                object->ClearIgnoreAccessFlag();
            }

            if (validObject) {
                object->SetIgnoreAccessFlag();
                size_t objectSize = object->SizeOf();
                object->ClearIgnoreAccessFlag();
                if (objectOffsetMap.find(object) == objectOffsetMap.end()) {
                    std::streampos offset = swapfile.tellp();
                    swapfile.write((char *)object, objectSize);
                    bool writeError = checkStreamError(swapfile,
                            "writing object to swapfile for the first time in WriteTask");
                    if (writeError) {
                        ioError = true;
                    }
                    objectOffsetMap[object] = offset;
                    objectSizeMap[object] = objectSize;
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
                        swapfile.write((char *)object, objectSize);
                        bool writeError = checkStreamError(swapfile,
                                "writing object to swapfile again in WriteTask");
                        if (writeError) {
                            ioError = true;
                        }
                        swapfile.seekp(curpos);
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
        }
        Locks::mutator_lock_->ReaderUnlock(self);

        if (numGarbageObjects > 0 || numNullClasses > 0 || numSpacelessObjects > 0
                || numResizedObjects > 0) {
            LOG(INFO) << "NIEL WriteTask irregularities: " << numGarbageObjects
                      << " garbage objects, " << numNullClasses << " null classes, "
                      << numSpacelessObjects << " objects not in any space, "
                      << numResizedObjects << " resized objects";
        }

        uint64_t nanoTime = NanoTime();
        uint64_t targetTime = nanoTime + 3000000000;
        Runtime * runtime = Runtime::Current();
        gc::Heap * curHeap = (runtime == nullptr ? nullptr : runtime->GetHeap());
        gc::TaskProcessor * taskProcessor =
            (curHeap == nullptr ? nullptr : curHeap->GetTaskProcessor());
        if (taskProcessor != nullptr && taskProcessor->IsRunning()) {
            if (ioError) {
                LOG(INFO) << "NIEL not scheduling WriteTask again due to IO error";
            }
            else {
                taskProcessor->AddTask(self, new WriteTask(targetTime));
            }
        }
    }
};


void StartAccessCount(gc::collector::GarbageCollector * gc) {
    if (errorCount > 0) {
        LOG(INFO) << "NIEL (GC " << gc->GetName() << "): error count " << errorCount << " on prev GC";
    }
    doingAccessCount = true;

    objectsRead = 0;
    objectsWritten = 0;
    objectsReadAndWritten = 0;
    totalObjects = 0;
    errorCount = 0;
    shenanigansCount = 0;

    readTotalObjectSize = 0;
    readTotalPointerSize = 0;
    unreadTotalObjectSize = 0;
    unreadTotalPointerSize = 0;
    smallObjectTotalObjectSize = 0;
    smallObjectTotalPointerSize = 0;
    largeObjectTotalObjectSize = 0;
    largeObjectTotalPointerSize = 0;

    smallObjectPointerFracHist.Clear();
    largeObjectPointerFracHist.Clear();
    readCountHist.Clear();
    writeCountHist.Clear();
    readShiftRegHist.Clear();
    writeShiftRegHist.Clear();

    readsVsPointerFracHist.Clear();
    writesVsPointerFracHist.Clear();
    readShiftRegVsPointerFracHist.Clear();
    writeShiftRegVsPointerFracHist.Clear();
    objectSizeVsPointerFracHist.Clear();

    std::string swapfilePath("/data/data/" + getPackageName() + "/swapfile");
    uint32_t curPid = getpid();
    if (curPid != pid) {
        pid = curPid;
        openFile(swapfilePath, swapfile);
        objectOffsetMap.clear();
        objectSizeMap.clear();
        writeQueue.clear();

        swapfile.write((char *)&pid, 4);
        swapfile.flush();
        bool ioError = checkStreamError(swapfile, "after opening swapfile");

        if (heap->GetTaskProcessor() != nullptr && heap->GetTaskProcessor()->IsRunning()) {
            if (ioError) {
                LOG(INFO) << "NIEL not scheduling first WriteTask due to IO error";
            }
            else {
                heap->GetTaskProcessor()->AddTask(Thread::Current(), new WriteTask(NanoTime()));
            }
        }
    }
}

void CountAccess(gc::collector::GarbageCollector * gc ATTRIBUTE_UNUSED, mirror::Object * object) {
    if (doingAccessCount) {
        object->SetIgnoreAccessFlag();
        size_t objectSize = object->SizeOf();
        mirror::Class * klass = object->GetClass();
        object->ClearIgnoreAccessFlag();
        uint32_t numPointers = klass->NumReferenceInstanceFields();
        mirror::Class * superClass = klass->GetSuperClass();
        while (superClass != nullptr) {
            numPointers += superClass->NumReferenceInstanceFields();
            superClass = superClass->GetSuperClass();
        }
        size_t sizeOfPointers = numPointers * sizeof(mirror::HeapReference<mirror::Object>);
        double pointerSizeFrac = ((double)sizeOfPointers) / objectSize;

        if (objectSize > 200) {
            largeObjectPointerFracHist.Add(pointerSizeFrac);
            largeObjectTotalPointerSize += sizeOfPointers;
            largeObjectTotalObjectSize += objectSize;
        }
        else {
            smallObjectPointerFracHist.Add(pointerSizeFrac);
            smallObjectTotalPointerSize += sizeOfPointers;
            smallObjectTotalObjectSize += objectSize;
        }

        uint32_t readCounterVal = object->GetReadCounter();
        uint32_t writeCounterVal = object->GetWriteCounter();

        bool wasRead = false;
        bool wasWritten = false;

        if (readCounterVal > 0) {
            object->ClearReadCounter();
            objectsRead += 1;
            wasRead = true;
            readTotalPointerSize += sizeOfPointers;
            readTotalObjectSize += objectSize;
        }
        else {
            unreadTotalPointerSize += sizeOfPointers;
            unreadTotalObjectSize += objectSize;
        }

        if (writeCounterVal > 0) {
            object->ClearWriteCounter();
            objectsWritten += 1;
            wasWritten = true;
        }

        if (wasRead && wasWritten) {
            objectsReadAndWritten += 1;
        }

        object->UpdateReadShiftRegister(wasRead);
        object->UpdateWriteShiftRegister(wasWritten);

        uint32_t rsrVal = object->GetReadShiftRegister();
        uint32_t wsrVal = object->GetWriteShiftRegister();

        readCountHist.Add(readCounterVal);
        writeCountHist.Add(writeCounterVal);
        readShiftRegHist.Add(rsrVal);
        writeShiftRegHist.Add(wsrVal);

        readsVsPointerFracHist.Add(readCounterVal, pointerSizeFrac);
        writesVsPointerFracHist.Add(readCounterVal, pointerSizeFrac);
        readShiftRegVsPointerFracHist.Add(rsrVal, pointerSizeFrac);
        writeShiftRegVsPointerFracHist.Add(wsrVal, pointerSizeFrac);
        objectSizeVsPointerFracHist.Add(objectSize, pointerSizeFrac);

        totalObjects += 1;

        if (objectSize > 1000) {
            Thread * self = Thread::Current();
            writeQueueMutex.ExclusiveLock(self);
            writeQueue.push_back(object);
            object->SetPadding(MAGIC_NUM);
            writeQueueMutex.ExclusiveUnlock(self);
        }
    }
    else {
        errorCount += 1;
    }
}

void FinishAccessCount(gc::collector::GarbageCollector * gc) {
    doingAccessCount = false;
    if (strstr(gc->GetName(), "partial concurrent mark sweep")) {
        LOG(INFO) << "NIEL (GC " << gc->GetName() << "): objects read: " << objectsRead
                  << " objects written: " << objectsWritten << " objects read and written: "
                  << objectsReadAndWritten << " total objects: " << totalObjects
                  << " shenanigans: " << shenanigansCount;
        LOG(INFO) << "NIEL total pointer frac of read objects: "
                  << (double)readTotalPointerSize / readTotalObjectSize
                  << " pointer size: " << readTotalPointerSize
                  << " object size: " << readTotalObjectSize;
        LOG(INFO) << "NIEL unread total pointer frac: "
                  << (double)unreadTotalPointerSize / unreadTotalObjectSize
                  << " pointer size: " << unreadTotalPointerSize
                  << " object size: " << unreadTotalObjectSize;
        LOG(INFO) << "NIEL small total pointer frac: "
                  << (double)smallObjectTotalPointerSize / smallObjectTotalObjectSize
                  << " pointer size: " << smallObjectTotalPointerSize
                  << " object size: " << smallObjectTotalObjectSize;
        LOG(INFO) << "NIEL large total pointer frac: "
                  << (double)largeObjectTotalPointerSize / largeObjectTotalObjectSize
                  << " pointer size: " << largeObjectTotalPointerSize
                  << " object size: " << largeObjectTotalObjectSize;
        LOG(INFO) << "NIEL pointer frac hist of small objects (scaled):\n"
                  << smallObjectPointerFracHist.Print(true, true);
        LOG(INFO) << "NIEL pointer frac hist of large objects (scaled):\n"
                  << largeObjectPointerFracHist.Print(true, true);
        LOG(INFO) << "NIEL read count hist:\n" << readCountHist.Print(false, true);
        LOG(INFO) << "NIEL write count hist:\n" << writeCountHist.Print(false, true);
        LOG(INFO) << "NIEL read shift register hist:\n" << readShiftRegHist.Print(false, true);
        LOG(INFO) << "NIEL write shift register hist:\n" << writeShiftRegHist.Print(false, true);
        LOG(INFO) << "NIEL read count vs pointer frac hist:\n"
                  << readsVsPointerFracHist.Print(false);
        LOG(INFO) << "NIEL write count vs pointer frac hist:\n"
                  << writesVsPointerFracHist.Print(false);
        LOG(INFO) << "NIEL read shift reg vs pointer frac hist:\n"
                  << readShiftRegVsPointerFracHist.Print(false);
        LOG(INFO) << "NIEL write shift reg vs pointer frac hist:\n"
                  << writeShiftRegVsPointerFracHist.Print(false);
        LOG(INFO) << "NIEL object size vs pointer frac hist:\n"
                  << objectSizeVsPointerFracHist.Print(false);
        LOG(INFO) << "NIEL package name: " << getPackageName();
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

void maybePrintLog() {
    time_t currentTime = time(NULL);
    if (difftime(currentTime, lastLogTime) > LOG_INTERVAL_SECONDS) {
        //printAllocCounts();
        //printHeap();
        lastLogTime = currentTime;
    }
}

void printAllocCounts() {
    for (auto it = currentAllocCounts.begin(); it != currentAllocCounts.end(); it++) {
        std::string name = it->first;
        LOG(INFO) << "NIEL space |" << name << "| curr count: " << currentAllocCounts[name]
                  << " curr size: " << currentAllocSizes[name] << " total count: "
                  << totalAllocCounts[name] << " total size: " << totalAllocSizes[name]
                  ;
    }
    LOG(INFO) << "NIEL RosAlloc curr thread-local/normal count: " << numCurrentRosAllocAllocs
              << " size: " << sizeCurrentRosAllocAllocs
              << " curr large count: " << numCurrentRosAllocLargeObjectAllocs
              << " size: " << sizeCurrentRosAllocLargeObjectAllocs
              ;
    LOG(INFO) << "NIEL RosAlloc total thread-local count: " << numTotalRosAllocThreadLocalAllocs
              << " size: " << sizeTotalRosAllocThreadLocalAllocs
              << " total normal count: " << numTotalRosAllocNormalAllocs
              << " size: " << sizeTotalRosAllocNormalAllocs
              << " total large count: " << numTotalRosAllocLargeObjectAllocs
              << " size: " << sizeTotalRosAllocLargeObjectAllocs
              ;
}

void printHeap() {
    if (heap == NULL) {
        return;
    }

    LOG(INFO) << "NIEL num spaces " << heap->nielinst_spaces_.size();
    for (auto it = heap->nielinst_spaces_.begin(); it != heap->nielinst_spaces_.end(); it++) {
        LOG(INFO) << "NIEL space " << (*it)->GetName()
                  << " continuous? " << (*it)->IsContinuousSpace()
                  << " discontinuous? " << (*it)->IsDiscontinuousSpace()
                  << " alloc? " << (*it)->IsAllocSpace()
                  ;
    }
    LOG(INFO) << "NIEL num garbage collectors " << heap->nielinst_GetGarbageCollectors()->size();
    for (auto it = heap->nielinst_GetGarbageCollectors()->begin(); it != heap->nielinst_GetGarbageCollectors()->end(); it++) {
        LOG(INFO) << "NIEL garbage collector " << (*it)->GetName()
                  << " semi_space? " << ((*it) == heap->nielinst_GetSemiSpace())
                  << " mark_compact? " << ((*it) == heap->nielinst_GetMarkCompact())
                  << " concurrent_copying? " << ((*it) == heap->nielinst_GetConcurrentCopying())
                  ;
    }

    gc::space::RosAllocSpace * rosAllocSpace = heap->GetRosAllocSpace();
    if (rosAllocSpace != NULL) {
        gc::allocator::RosAlloc * rosAlloc = rosAllocSpace->GetRosAlloc();
        if (rosAlloc != NULL) {
            LOG(INFO) << "NIEL RosAlloc footprint: " << rosAlloc->Footprint();
        }
    }
}

} // namespace nielinst
} // namespace art
