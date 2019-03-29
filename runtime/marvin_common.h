#ifndef ART_RUNTIME_MARVIN_COMMON_H_
#define ART_RUNTIME_MARVIN_COMMON_H_

/*
 * Common utility methods shared across marvin_swap and marvin_instrumentation.
 */

#include <fstream>
#include <string>

#include "base/mutex.h"
#include "gc/heap-inl.h"
#include "mirror/object.h"
#include "runtime.h"

namespace art {

namespace marvin {

void openFile(const std::string & path, std::fstream & stream);

void openFileAppend(const std::string & path, std::fstream & stream);

bool checkStreamError(const std::ios & stream, const std::string & msg);

std::string getPackageName();

bool appOnCommonBlacklist(const std::string & packageName);

/*
 * Returns true if the object is cold enough to be swapped based on the given
 * data.
 */
inline bool objectIsCold(uint8_t writeShiftRegVal, bool wasWritten) {
    return writeShiftRegVal < 2 && !wasWritten;
}

inline bool objectIsLarge(size_t objectSize) {
    return objectSize >= 2 * 1024;
}

/*
 * Check if an object is of a type for which swapping is enabled.
 *
 * NOTE: This function performs a read on the object and does not set or clear
 * the object's IgnoreReadFlag; you should set and clear that flag if you want
 * to avoid counting the read.
 */
inline bool objectIsSwappableType(mirror::Object * obj)
        SHARED_REQUIRES(Locks::mutator_lock_) {
    return (
               (obj->IsArrayInstance() && !obj->IsObjectArray())
            || (!obj->IsArrayInstance() && !obj->IsClass() && !obj->IsClassLoader()
                 && !obj->IsDexCache() && !obj->IsString() && !obj->IsReferenceInstance())
            );
}

inline gc::Heap * getHeapChecked() {
    Runtime * runtime = Runtime::Current();
    if (runtime == nullptr) {
        return nullptr;
    }
    return runtime->GetHeap();
}

} // namespace marvin
} // namespace art

#endif // ART_RUNTIME_MARVIN_COMMON_H_
