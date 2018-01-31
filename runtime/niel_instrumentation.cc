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
#include <map>

#include "niel_histogram.h"

namespace art {

namespace nielinst {

/* Constants */
const int LOG_INTERVAL_SECONDS = 10;

/* Internal functions */
void maybePrintLog();
void printAllocCounts();
void printHeap();

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

long objectsAccessed = 0;
long totalObjects = 0;
long errorCount = 0;
long shenanigansCount = 0; // used for misc debugging

long accessedTotalObjectSize = 0;
long accessedTotalPointerSize = 0;
long untouchedTotalObjectSize = 0;
long untouchedTotalPointerSize = 0;
long smallObjectTotalObjectSize = 0;
long smallObjectTotalPointerSize = 0;
long largeObjectTotalObjectSize = 0;
long largeObjectTotalPointerSize = 0;

Histogram accessedPointerFracHist(10, 0, 1);
Histogram untouchedPointerFracHist(10, 0, 1);
Histogram accessedObjectSizeHist(10, 0, 400);
Histogram untouchedObjectSizeHist(10, 0, 400);
Histogram smallObjectPointerFracHist(10, 0, 1); // objects <=200 bytes
Histogram largeObjectPointerFracHist(10, 0, 1); // objects >200 bytes

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

    objectsAccessed = 0;
    totalObjects = 0;
    errorCount = 0;
    shenanigansCount = 0;

    accessedTotalObjectSize = 0;
    accessedTotalPointerSize = 0;
    untouchedTotalObjectSize = 0;
    untouchedTotalPointerSize = 0;
    smallObjectTotalObjectSize = 0;
    smallObjectTotalPointerSize = 0;
    largeObjectTotalObjectSize = 0;
    largeObjectTotalPointerSize = 0;

    accessedPointerFracHist.Clear();
    untouchedPointerFracHist.Clear();
    accessedObjectSizeHist.Clear();
    untouchedObjectSizeHist.Clear();
    smallObjectPointerFracHist.Clear();
    largeObjectPointerFracHist.Clear();
}

void CountAccess(mirror::Object * object) SHARED_REQUIRES(Locks::mutator_lock_) {
    if (doingAccessCount) {
        object->SetAccessBit(1);
        size_t objectSize = object->SizeOf();
        mirror::Class * klass = object->GetClass();
        object->ClearAccessBit(1);
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

        if (object->GetAccessBit(0)) {
            object->ClearAccessBit(0);
            objectsAccessed += 1;
            accessedPointerFracHist.Add(pointerSizeFrac);
            accessedObjectSizeHist.Add(objectSize);
            accessedTotalPointerSize += sizeOfPointers;
            accessedTotalObjectSize += objectSize;
        }
        else {
            untouchedPointerFracHist.Add(pointerSizeFrac);
            untouchedObjectSizeHist.Add(objectSize);
            untouchedTotalPointerSize += sizeOfPointers;
            untouchedTotalObjectSize += objectSize;
        }
        totalObjects += 1;
    }
    else {
        errorCount += 1;
    }
}

void FinishAccessCount(gc::collector::GarbageCollector * gc) {
    doingAccessCount = false;
    if (strstr(gc->GetName(), "partial concurrent mark sweep")) {
        LOG(INFO) << "NIEL (GC " << gc->GetName() << "): objects accessed: "
                  << objectsAccessed << " total objects: " << totalObjects
                  << " shenanigans: " << shenanigansCount;
        LOG(INFO) << "NIEL accessed total pointer frac: "
                  << (double)accessedTotalPointerSize / accessedTotalObjectSize
                  << " pointer size: " << accessedTotalPointerSize
                  << " object size: " << accessedTotalObjectSize;
        LOG(INFO) << "NIEL untouched total pointer frac: "
                  << (double)untouchedTotalPointerSize / untouchedTotalObjectSize
                  << " pointer size: " << untouchedTotalPointerSize
                  << " object size: " << untouchedTotalObjectSize;
        LOG(INFO) << "NIEL small total pointer frac: "
                  << (double)smallObjectTotalPointerSize / smallObjectTotalObjectSize
                  << " pointer size: " << smallObjectTotalPointerSize
                  << " object size: " << smallObjectTotalObjectSize;
        LOG(INFO) << "NIEL large total pointer frac: "
                  << (double)largeObjectTotalPointerSize / largeObjectTotalObjectSize
                  << " pointer size: " << largeObjectTotalPointerSize
                  << " object size: " << largeObjectTotalObjectSize;
        LOG(INFO) << "NIEL pointer frac hist of accessed objects (scaled):\n"
                  << accessedPointerFracHist.Print(true, true);
        LOG(INFO) << "NIEL pointer frac hist of untouched objects (scaled):\n"
                  << untouchedPointerFracHist.Print(true, true);
        LOG(INFO) << "NIEL object size hist of accessed objects(scaled):\n"
                  << accessedObjectSizeHist.Print(true, true);
        LOG(INFO) << "NIEL object size hist of untouched objects(scaled):\n"
                  << untouchedObjectSizeHist.Print(true, true);
        LOG(INFO) << "NIEL pointer frac hist of small objects (scaled):\n"
                  << smallObjectPointerFracHist.Print(true, true);
        LOG(INFO) << "NIEL pointer frac hist of large objects (scaled):\n"
                  << largeObjectPointerFracHist.Print(true, true);
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
