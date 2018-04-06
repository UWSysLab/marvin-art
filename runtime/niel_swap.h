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

class Stub;

void GcRecordFree(Thread * self, mirror::Object * object);

/*
 * Open swap file and schedule initial WriteTask, if this process has
 * just forked from the zygote.
 */
void InitIfNecessary(Thread * self);

void CompactSwapFile(Thread * self);

/*
 * Check if an object should be written to disk and then update its bookkeeping
 * state.
 */
void CheckAndUpdate(gc::collector::GarbageCollector * gc, mirror::Object * object)
    SHARED_REQUIRES(Locks::mutator_lock_);

/*
 * Swap all live objects in the swap file back into memory.
 */
void SwapObjectsIn(gc::Heap * heap) REQUIRES(Locks::mutator_lock_);

/*
 * Swap all objects in the swapOutQueue out to disk by replacing them with
 * stubs, freeing them from memory, patching references to point to the stubs
 * rather than the objects, and remapping the swap data structures to use
 * pointers to the stubs.
 */
void SwapObjectsOut(Thread * self, gc::Heap * heap) REQUIRES(Locks::mutator_lock_);

/*
 * Called by semi-space GC to tell us where an object is moving.
 */
void RecordForwardedObject(Thread * self, mirror::Object * obj, mirror::Object * forwardAddress);

/*
 * Update bookkeeping data structures to have correct pointers after
 * semi-space GC.
 */
void SemiSpaceUpdateDataStructures(Thread * self);

/*
 * Swaps in an object on-demand. Installs a pointer to the object in the stub
 * so that method calls can be redirected to the object until references are
 * patched in CleanUpOnDemandSwaps().
 */
void SwapInOnDemand(Stub * stub) SHARED_REQUIRES(Locks::mutator_lock_);

/*
 * Cleans up state associated with objects that were swapped in on-demand by
 * patching references to point to the objects rather than the stubs, remapping
 * the swap data structures to use the object pointers instead of the stub
 * pointers, and freeing the stubs.
 */
void CleanUpOnDemandSwaps(gc::Heap * heap) REQUIRES(Locks::mutator_lock_);

} // namespace swap
} // namespace niel
} // namespace art

#endif
