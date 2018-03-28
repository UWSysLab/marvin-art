#ifndef ART_RUNTIME_NIEL_STUB_H_
#define ART_RUNTIME_NIEL_STUB_H_

#include <atomic>

#include "base/macros.h"
#include "base/mutex.h"
#include "globals.h"

namespace art {

namespace mirror {
    template <typename MirrorType> class HeapReference;
    class Object;
}

namespace niel {

namespace swap {

class Stub {
  public:
    static size_t GetStubSize(int numRefs);

    void Init(int numRefs) SHARED_REQUIRES(Locks::mutator_lock_);

    void SetReference(int pos, mirror::Object * ref) SHARED_REQUIRES(Locks::mutator_lock_);
    mirror::Object * GetReference(int pos) SHARED_REQUIRES(Locks::mutator_lock_);
    mirror::HeapReference<mirror::Object> * GetReferenceAddress(int pos)
        SHARED_REQUIRES(Locks::mutator_lock_);

    // Copied from mirror/object.h
    bool GetStubFlag();

    void Dump();

  private:
    // Copied from mirror/object.h
    static uint8_t GetBitsAtomic8(const std::atomic<uint8_t> & data, uint8_t offset,
                                uint8_t width, std::memory_order order);
    static void SetBitsAtomic8(std::atomic<uint8_t> & data, uint8_t offset, uint8_t width,
                             std::memory_order order);
    static void ClearBitsAtomic8(std::atomic<uint8_t> & data, uint8_t offset, uint8_t width,
                               std::memory_order order);

    void SetStubFlag();
    void ClearFlags();

    uint32_t padding_a_;
    uint32_t padding_b_;

    std::atomic<uint8_t> x_flags_;
    uint8_t padding_c_;
    uint16_t num_refs_;

    uint32_t padding_d_;
};

} // namespace swap
} // namespace niel
} // namespace art

#endif // ART_RUNTIME_NIEL_STUB_H_
