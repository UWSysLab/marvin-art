#include "niel_instrumentation.h"

#include "base/mutex.h"
#include "gc/allocator/rosalloc.h"
#include "gc/collector/garbage_collector.h"
#include "gc/heap.h"
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
#include <unistd.h>
#include <sys/types.h>

#include "niel_bivariate_histogram.h"
#include "niel_histogram.h"

namespace art {

namespace nielinst {

/* Constants */
const int LOG_INTERVAL_SECONDS = 10;

/* Internal functions */
void maybePrintLog();
void printAllocCounts();
void printHeap();
std::string getPackageName();
void openSwapFileIfNotOpen();

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

    openSwapFileIfNotOpen();
}

void CountAccess(mirror::Object * object) SHARED_REQUIRES(Locks::mutator_lock_) {
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

        char data[4];
        data[0] = 0;
        data[1] = 1;
        data[2] = 2;
        data[3] = 255;
        swapfile.write(data, 4);
        swapfile.flush();
    }
}

std::string getPackageName() {
    std::ifstream cmdlineFile("/proc/self/cmdline");
    std::string cmdline;
    getline(cmdlineFile, cmdline);
    cmdlineFile.close();
    return cmdline.substr(0, cmdline.find((char)0));
}

void openSwapFileIfNotOpen() {
    if (!swapfile.is_open()) {
        std::string filename("/data/data/" + getPackageName() + "/swapfile");
        swapfile.open(filename, std::ios::binary | std::ios::in | std::ios::out);
        if (!swapfile) {
            LOG(INFO) << "NIEL opening swapfile in write-only mode to create file";
            swapfile.close();
            swapfile.open(filename, std::ios::binary | std::ios::out);
            swapfile.close();
            swapfile.open(filename, std::ios::binary | std::ios::in | std::ios::out);
            if (!swapfile) {
                LOG(INFO) << "NIEL error creating swapfile";
            }
        }
    }
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
