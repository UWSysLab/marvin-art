#ifndef ART_RUNTIME_NIEL_SWAP_H_
#define ART_RUNTIME_NIEL_SWAP_H_

#include "base/mutex.h"

namespace art {

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
 * Check if an object should be written to disk and then update its bookeeping
 * state.
 */
void CheckAndUpdate(mirror::Object * object) SHARED_REQUIRES(Locks::mutator_lock_);

/*
 * Lock and unlock all objects. Used during WriteTask::Run(),
 * mirror::Object::SetField(), and mirror::SetFieldObjectWithoutWriteBarrier()
 * to ensure that an object is not modified while WriteTask is taking a
 * snapshot of it.
 */
void LockObjects();
void UnlockObjects();

} // namespace swap
} // namespace niel
} // namespace art

#endif
