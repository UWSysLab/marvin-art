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

enum NiAllocType {
    NI_ALLOC_ROSALLOC_THREAD_LOCAL, NI_ALLOC_ROSALLOC_NORMAL, NI_ALLOC_ROSALLOC_LARGE,
    NI_ALLOC_DLMALLOC, NI_ALLOC_LOS
};

enum NiFreeType {
    NI_FREE_ROSALLOC, NI_FREE_ROSALLOC_LARGE, NI_FREE_DLMALLOC, NI_FREE_LOS
};

void NiRecordAlloc(Thread * self, size_t size, NiAllocType type);
void NiRecordFree(Thread * self, size_t size, NiFreeType type);

void NiSetHeap(gc::Heap * inHeap);

void maybePrintLog();
void printHeap();

}

#endif
