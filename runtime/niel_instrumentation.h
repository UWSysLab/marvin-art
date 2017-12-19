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

enum NiRosAllocAllocType {
    NI_ROSALLOC_ALLOC_THREAD_LOCAL, NI_ROSALLOC_ALLOC_NORMAL, NI_ROSALLOC_ALLOC_LARGE
};

enum NiRosAllocFreeType {
    NI_ROSALLOC_FREE_NORMAL_OR_THREAD_LOCAL, NI_ROSALLOC_FREE_LARGE
};

void NiRecordRosAllocAlloc(Thread * self, size_t size, NiRosAllocAllocType type);
void NiRecordRosAllocFree(Thread * self, size_t size, NiRosAllocFreeType type);

void NiRecordAlloc(Thread * self, gc::space::Space * space, size_t size);
void NiRecordFree(Thread * self, gc::space::Space * space, size_t size, int count);

void NiSetHeap(gc::Heap * inHeap);

void maybePrintLog();
void printHeap();

}

#endif
