#include "niel_instrumentation.h"

#include "base/mutex.h"
#include "gc/allocator/rosalloc.h"
#include "gc/heap.h"
#include "gc/space/rosalloc_space.h"
#include "gc/space/space.h"
#include "globals.h"
#include "thread.h"

#include <ctime>
#include <map>

namespace art {

const int LOG_INTERVAL_SECONDS = 5;

Mutex instMutex("NielInstrumentationMutex", kLoggingLock);

gc::Heap * heap = NULL;

time_t lastLogTime;

/* Locked with instMutex */
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

/* End locked with instMutex */

void NiRecordRosAllocAlloc(Thread * self, size_t size, NiRosAllocAllocType type) {
    instMutex.ExclusiveLock(self);

    switch(type) {
      case NI_ROSALLOC_ALLOC_THREAD_LOCAL:
        numTotalRosAllocThreadLocalAllocs++;
        sizeTotalRosAllocThreadLocalAllocs += size;
        numCurrentRosAllocAllocs++;
        sizeCurrentRosAllocAllocs += size;
        break;
      case NI_ROSALLOC_ALLOC_NORMAL:
        numTotalRosAllocNormalAllocs++;
        sizeTotalRosAllocNormalAllocs += size;
        numCurrentRosAllocAllocs++;
        sizeCurrentRosAllocAllocs += size;
        break;
      case NI_ROSALLOC_ALLOC_LARGE:
        numTotalRosAllocLargeObjectAllocs++;
        sizeTotalRosAllocLargeObjectAllocs += size;
        numCurrentRosAllocLargeObjectAllocs++;
        sizeCurrentRosAllocLargeObjectAllocs += size;
        break;
    }

    maybePrintLog();
    instMutex.ExclusiveUnlock(self);
}

void NiRecordRosAllocFree(Thread * self, size_t size, NiRosAllocFreeType type) {
    instMutex.ExclusiveLock(self);

    switch(type) {
      case NI_ROSALLOC_FREE_NORMAL_OR_THREAD_LOCAL:
        numCurrentRosAllocAllocs--;
        sizeCurrentRosAllocAllocs -= size;
        break;
      case NI_ROSALLOC_FREE_LARGE:
        numCurrentRosAllocLargeObjectAllocs--;
        sizeCurrentRosAllocLargeObjectAllocs -= size;
        break;
    }

    maybePrintLog();
    instMutex.ExclusiveUnlock(self);
}

void NiRecordAlloc(Thread * self, gc::space::Space * space, size_t size) {
    instMutex.ExclusiveLock(self);

    std::string name(space->GetName());
    currentAllocCounts[name]++;
    currentAllocSizes[name] += size;
    totalAllocCounts[name]++;
    totalAllocSizes[name] += size;

    maybePrintLog();
    instMutex.ExclusiveUnlock(self);
}

void NiRecordFree(Thread * self, gc::space::Space * space, size_t size, int count) {
    instMutex.ExclusiveLock(self);

    std::string name(space->GetName());
    currentAllocCounts[name] -= count;
    currentAllocSizes[name] -= size;

    maybePrintLog();
    instMutex.ExclusiveUnlock(self);
}

void NiSetHeap(gc::Heap * inHeap) {
    heap = inHeap;
}

void maybePrintLog() {
    time_t currentTime = time(NULL);
    if (difftime(currentTime, lastLogTime) > LOG_INTERVAL_SECONDS) {
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
        //printHeap();
        lastLogTime = currentTime;
    }
}

void printHeap() {
    if (heap == NULL) {
        return;
    }

    LOG(INFO) << "NIEL num spaces " << heap->ni_spaces_.size();
    for (auto it = heap->ni_spaces_.begin(); it != heap->ni_spaces_.end(); it++) {
        LOG(INFO) << "NIEL space " << (*it)->GetName()
                  << " continuous? " << (*it)->IsContinuousSpace()
                  << " discontinuous? " << (*it)->IsDiscontinuousSpace()
                  << " alloc? " << (*it)->IsAllocSpace()
                  ;
    }
    LOG(INFO) << "NIEL num garbage collectors " << heap->NiGetGarbageCollectors()->size();
    for (auto it = heap->NiGetGarbageCollectors()->begin(); it != heap->NiGetGarbageCollectors()->end(); it++) {
        LOG(INFO) << "NIEL garbage collector " << (*it)->GetName()
                  << " semi_space? " << ((*it) == heap->NiGetSemiSpace())
                  << " mark_compact? " << ((*it) == heap->NiGetMarkCompact())
                  << " concurrent_copying? " << ((*it) == heap->NiGetConcurrentCopying())
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

}
