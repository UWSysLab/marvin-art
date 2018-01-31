#ifndef ART_RUNTIME_NIEL_INSTRUMENTATION_H_
#define ART_RUNTIME_NIEL_INSTRUMENTATION_H_

#include "globals.h"

namespace art {

namespace gc {
    class Heap;
    namespace collector {
        class GarbageCollector;
    }
    namespace space {
        class Space;
    }
}
namespace mirror {
    class Object;
}
class Thread;

namespace nielinst {

enum RosAllocAllocType {
    ROSALLOC_ALLOC_THREAD_LOCAL, ROSALLOC_ALLOC_NORMAL, ROSALLOC_ALLOC_LARGE
};

enum RosAllocFreeType {
    ROSALLOC_FREE_NORMAL_OR_THREAD_LOCAL, ROSALLOC_FREE_LARGE
};

void RecordRosAllocAlloc(Thread * self, size_t size, RosAllocAllocType type);
void RecordRosAllocFree(Thread * self, size_t size, RosAllocFreeType type);

void RecordAlloc(Thread * self, gc::space::Space * space, size_t size);
void RecordFree(Thread * self, gc::space::Space * space, size_t size, int count);

void SetHeap(gc::Heap * inHeap);

void StartAccessCount(gc::collector::GarbageCollector * gc);
void CountAccess(mirror::Object * object);
void FinishAccessCount(gc::collector::GarbageCollector * gc);

} // namespace nielinst
} // namespace art

#endif
