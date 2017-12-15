#ifndef ART_RUNTIME_NIEL_INSTRUMENTATION_H_
#define ART_RUNTIME_NIEL_INSTRUMENTATION_H_

#include "globals.h"

namespace art {

namespace gc {
    class Heap;
    namespace space {
        class Space;
    }
}
class Thread;

void NiRecordRosAllocThreadLocalAlloc(Thread * self, size_t size);
void NiRecordRosAllocNormalAlloc(Thread * self, size_t size);
void NiRecordRosAllocLargeObjectAlloc(Thread * self, size_t size);
void NiRecordLargeObjectAlloc(Thread * self, size_t size);

void NiSetHeap(gc::Heap * inHeap);

void maybePrintLog();
void printHeap();

}

#endif
