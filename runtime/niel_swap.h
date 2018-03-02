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

/*
 * Update an object's bookkeeping state and check if it should be
 * written to disk.
 */
void UpdateAndCheck(mirror::Object * object) SHARED_REQUIRES(Locks::mutator_lock_);

} // namespace swap
} // namespace niel
} // namespace art

#endif
