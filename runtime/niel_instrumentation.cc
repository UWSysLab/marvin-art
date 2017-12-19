#include "niel_instrumentation.h"

#include "base/mutex.h"
#include "gc/allocator/rosalloc.h"
#include "gc/heap.h"
#include "gc/space/rosalloc_space.h"
#include "gc/space/space.h"
#include "globals.h"
#include "thread.h"

#include <ctime>

namespace art {

const int LOG_INTERVAL_SECONDS = 5;

Mutex instMutex("NielInstrumentationMutex", kLoggingLock);

gc::Heap * heap = NULL;

time_t lastLogTime;

/* Locked with instMutex */
long numTotalRosAllocThreadLocalAllocs = 0;
long numTotalRosAllocNormalAllocs = 0;
long numTotalRosAllocLargeObjectAllocs = 0;
long numTotalDlMallocAllocs = 0;
long numTotalLargeObjectAllocs = 0;

long sizeTotalRosAllocThreadLocalAllocs = 0;
long sizeTotalRosAllocNormalAllocs = 0;
long sizeTotalRosAllocLargeObjectAllocs = 0;
long sizeTotalDlMallocAllocs = 0;
long sizeTotalLargeObjectAllocs = 0;

long numCurrentRosAllocAllocs = 0;
long numCurrentRosAllocLargeObjectAllocs = 0;
long numCurrentDlMallocAllocs = 0;
long numCurrentLargeObjectAllocs = 0;

long sizeCurrentRosAllocAllocs = 0;
long sizeCurrentRosAllocLargeObjectAllocs = 0;
long sizeCurrentDlMallocAllocs = 0;
long sizeCurrentLargeObjectAllocs = 0;
/* End locked with instMutex */

void NiRecordAlloc(Thread * self, size_t size, NiAllocType type) {
    instMutex.ExclusiveLock(self);

    switch(type) {
      case NI_ALLOC_ROSALLOC_THREAD_LOCAL:
        numTotalRosAllocThreadLocalAllocs++;
        sizeTotalRosAllocThreadLocalAllocs += size;
        numCurrentRosAllocAllocs++;
        sizeCurrentRosAllocAllocs += size;
        break;
      case NI_ALLOC_ROSALLOC_NORMAL:
        numTotalRosAllocNormalAllocs++;
        sizeTotalRosAllocNormalAllocs += size;
        numCurrentRosAllocAllocs++;
        sizeCurrentRosAllocAllocs += size;
        break;
      case NI_ALLOC_ROSALLOC_LARGE:
        numTotalRosAllocLargeObjectAllocs++;
        sizeTotalRosAllocLargeObjectAllocs += size;
        numCurrentRosAllocLargeObjectAllocs++;
        sizeCurrentRosAllocLargeObjectAllocs += size;
        break;
      case NI_ALLOC_DLMALLOC:
        numTotalDlMallocAllocs++;
        sizeTotalDlMallocAllocs += size;
        numCurrentDlMallocAllocs++;
        sizeCurrentDlMallocAllocs += size;
        break;
      case NI_ALLOC_LOS:
        numTotalLargeObjectAllocs++;
        sizeTotalLargeObjectAllocs += size;
        numCurrentLargeObjectAllocs++;
        sizeCurrentLargeObjectAllocs += size;
        break;
    }

    instMutex.ExclusiveUnlock(self);
    maybePrintLog();
}

void NiRecordFree(Thread * self, size_t size, NiFreeType type) {
    instMutex.ExclusiveLock(self);

    switch(type) {
      case NI_FREE_ROSALLOC:
        numCurrentRosAllocAllocs--;
        sizeCurrentRosAllocAllocs -= size;
        break;
      case NI_FREE_ROSALLOC_LARGE:
        numCurrentRosAllocLargeObjectAllocs--;
        sizeCurrentRosAllocLargeObjectAllocs -= size;
        break;
      case NI_FREE_DLMALLOC:
        numCurrentDlMallocAllocs--;
        sizeCurrentDlMallocAllocs -= size;
        break;
      case NI_FREE_LOS:
        numCurrentLargeObjectAllocs--;
        sizeCurrentLargeObjectAllocs -= size;
        break;
    }

    instMutex.ExclusiveUnlock(self);
    maybePrintLog();
}

void NiSetHeap(gc::Heap * inHeap) {
    heap = inHeap;
}

void maybePrintLog() {
    time_t currentTime = time(NULL);
    if (difftime(currentTime, lastLogTime) > LOG_INTERVAL_SECONDS) {
        LOG(INFO) << "NIEL total RosAlloc thread-local allocs: " << numTotalRosAllocThreadLocalAllocs
                  << " size: " << sizeTotalRosAllocThreadLocalAllocs
                  << "\n"
                  << "     total RosAlloc normal allocs: " << numTotalRosAllocNormalAllocs
                  << " size: " << sizeTotalRosAllocNormalAllocs
                  << "\n"
                  << "     total RosAlloc large object allocs: " << numTotalRosAllocLargeObjectAllocs
                  << " size: " << sizeTotalRosAllocLargeObjectAllocs
                  << "\n"
                  << "     total DlMalloc allocs: " << numTotalDlMallocAllocs
                  << " size: " << sizeTotalDlMallocAllocs
                  << "\n"
                  << "     total LargeObjectSpace allocs: " << numTotalLargeObjectAllocs
                  << " size: " << sizeTotalLargeObjectAllocs
                  ;
        LOG(INFO) << "NIEL current RosAlloc thread-local/normal allocs: " << numCurrentRosAllocAllocs
                  << " size: " << sizeCurrentRosAllocAllocs
                  << "\n"
                  << "     current RosAlloc large object allocs: " << numCurrentRosAllocLargeObjectAllocs
                  << " size: " << sizeCurrentRosAllocLargeObjectAllocs
                  << "\n"
                  << "     current DlMalloc allocs: " << numCurrentDlMallocAllocs
                  << " size: " << sizeCurrentDlMallocAllocs
                  << "\n"
                  << "     current LargeObjectSpace allocs: " << numCurrentLargeObjectAllocs
                  << " size: " << sizeCurrentLargeObjectAllocs
                  ;
        printHeap();
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
