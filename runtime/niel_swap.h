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
 * Swap all live objects in the swap file back into memory. Installs pointers
 * to the objects in their stubs so that method calls can be redirected to the
 * objects.
 */
void SwapObjectsIn(gc::Heap * heap) REQUIRES(Locks::mutator_lock_);

/*
 * Create stubs for all "swap candidate" objects that do not already have
 * stubs, move the objects into a separate memory space, patch references to
 * point to the stubs rather than the objects, and remap the swap data
 * structures to use pointers to the stubs.
 */
void CreateStubs(Thread * self, gc::Heap * heap) REQUIRES(Locks::mutator_lock_);

/*
 * Reclaim (free from memory) all objects with stubs that are not dirty and
 * cold.
 */
void SwapObjectsOut(Thread * self) REQUIRES(Locks::mutator_lock_);

/*
 * Called by semi-space GC to tell us where an object is moving.
 */
void RecordForwardedObject(Thread * self, mirror::Object * obj, mirror::Object * forwardAddress);

/*
 * Iterate through all live stubs and record the objects to which they point.
 * This information is used to free those objects if their corresponding stubs
 * not forwarded by the semi-space GC. This method should be called before
 * running the semi-space GC.
 */
void SemiSpaceRecordStubMappings(Thread * self) REQUIRES(Locks::mutator_lock_);

/*
 * Update bookkeeping data structures to have correct pointers after
 * semi-space GC.
 */
void SemiSpaceUpdateDataStructures(Thread * self);

/*
 * Swaps in an object on-demand. Installs a pointer to the object in the stub
 * so that method calls can be redirected to the object.
 */
void SwapInOnDemand(Stub * stub) SHARED_REQUIRES(Locks::mutator_lock_);

/*
 * Used by Heap::UpdateProcessState() to notify the swap code when the app
 * transitions into and out of the foreground state.
 */
void SetInForeground(bool inForeground);

/*
 * Set the app lock counter to 0 for every entry in the reclamation table.
 */
void UnlockAllReclamationTableEntries() REQUIRES(Locks::mutator_lock_);


// Not a part of public swap API. Utility function that would be in
// niel_common.h, except it needs to have access to the swappedInSpace.
bool objectInSwappableSpace(gc::Heap * heap, mirror::Object * obj);

} // namespace swap
} // namespace niel
} // namespace art

#endif
