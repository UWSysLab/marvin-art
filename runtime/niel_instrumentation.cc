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
long numRosAllocThreadLocalAllocs = 0;
long numRosAllocNormalAllocs = 0;
long numRosAllocLargeObjectAllocs = 0;
long numDlMallocAllocs = 0;
long numLargeObjectAllocs = 0;

long sizeRosAllocThreadLocalAllocs = 0;
long sizeRosAllocNormalAllocs = 0;
long sizeRosAllocLargeObjectAllocs = 0;
long sizeDlMallocAllocs = 0;
long sizeLargeObjectAllocs = 0;
/* End locked with instMutex */

void NiRecordRosAllocThreadLocalAlloc(Thread * self, size_t size) {
    instMutex.ExclusiveLock(self);
    numRosAllocThreadLocalAllocs++;
    sizeRosAllocThreadLocalAllocs += size;
    instMutex.ExclusiveUnlock(self);
    maybePrintLog();
}

void NiRecordRosAllocNormalAlloc(Thread * self, size_t size) {
    instMutex.ExclusiveLock(self);
    numRosAllocNormalAllocs++;
    sizeRosAllocNormalAllocs += size;
    instMutex.ExclusiveUnlock(self);
    maybePrintLog();
}

void NiRecordRosAllocLargeObjectAlloc(Thread * self, size_t size) {
    instMutex.ExclusiveLock(self);
    numRosAllocLargeObjectAllocs++;
    sizeRosAllocLargeObjectAllocs += size;
    instMutex.ExclusiveUnlock(self);
    maybePrintLog();
}

void NiRecordDlMallocAlloc(Thread * self, size_t size) {
    instMutex.ExclusiveLock(self);
    numDlMallocAllocs++;
    sizeDlMallocAllocs += size;
    instMutex.ExclusiveUnlock(self);
    maybePrintLog();
}

void NiRecordLargeObjectAlloc(Thread * self, size_t size) {
    instMutex.ExclusiveLock(self);
    numLargeObjectAllocs++;
    sizeLargeObjectAllocs += size;
    instMutex.ExclusiveUnlock(self);
    maybePrintLog();
}

void NiSetHeap(gc::Heap * inHeap) {
    heap = inHeap;
}

void maybePrintLog() {
    time_t currentTime = time(NULL);
    if (difftime(currentTime, lastLogTime) > LOG_INTERVAL_SECONDS) {
        LOG(INFO) << "NIEL total RosAlloc thread-local allocs: " << numRosAllocThreadLocalAllocs
                  << " size: " << sizeRosAllocThreadLocalAllocs
                  << "\n"
                  << "     total RosAlloc normal allocs: " << numRosAllocNormalAllocs
                  << " size: " << sizeRosAllocNormalAllocs
                  << "\n"
                  << "     total RosAlloc large object allocs: " << numRosAllocLargeObjectAllocs
                  << " size: " << sizeRosAllocLargeObjectAllocs
                  << "\n"
                  << "     total DlMalloc allocs: " << numDlMallocAllocs
                  << " size: " << sizeDlMallocAllocs
                  << "\n"
                  << "     total LargeObjectSpace allocs: " << numLargeObjectAllocs
                  << " size: " << sizeLargeObjectAllocs
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
