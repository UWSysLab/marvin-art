#ifndef ART_RUNTIME_NIEL_SWAP_H_
#define ART_RUNTIME_NIEL_SWAP_H_

#include "base/mutex.h"

namespace art {

namespace gc {
    namespace collector {
        class GarbageCollector;
    }

    class Heap;
}

namespace mirror {
    class Object;
}

namespace niel {

namespace swap {

void GcRecordFree(Thread * self, mirror::Object * object);

/*
 * Open swap file and schedule initial WriteTask, if this process has
 * just forked from the zygote.
 */
void InitIfNecessary();

void CompactSwapFile();

/*
 * Check if an object should be written to disk and then update its bookkeeping
 * state.
 */
void CheckAndUpdate(gc::collector::GarbageCollector * gc, mirror::Object * object)
    SHARED_REQUIRES(Locks::mutator_lock_);

/*
 * Delete objects that have been written to disk and replace them with stubs.
 */
void ReplaceObjectsWithStubs(Thread * self, gc::Heap * heap) REQUIRES(Locks::mutator_lock_);

/*
 * Walk all of the memory spaces and replace any references to swapped-out
 * objects with references to their corresponding stubs.
 */
void PatchStubReferences(Thread * self, gc::Heap * heap) REQUIRES(Locks::mutator_lock_);

/*
 * Called by semi-space GC to tell us where an object is moving.
 */
void RecordForwardedObject(mirror::Object * obj, mirror::Object * forwardAddress);

/*
 * Update bookkeeping data structures to have correct pointers after
 * semi-space GC.
 */
void SemiSpaceUpdateDataStructures();

} // namespace swap
} // namespace niel
} // namespace art

#endif
