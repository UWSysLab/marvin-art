#ifndef ART_RUNTIME_NIEL_STUB_H_
#define ART_RUNTIME_NIEL_STUB_H_

#include <atomic>

#include "base/macros.h"
#include "base/mutex.h"
#include "globals.h"

#include "niel_reclamation_table.h"

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
    static size_t GetStubSize(mirror::Object * object) SHARED_REQUIRES(Locks::mutator_lock_);

    void PopulateFrom(mirror::Object * object) SHARED_REQUIRES(Locks::mutator_lock_);
    void CopyRefsInto(mirror::Object * object) SHARED_REQUIRES(Locks::mutator_lock_);

    void SetReference(int pos, mirror::Object * ref) SHARED_REQUIRES(Locks::mutator_lock_);
    mirror::Object * GetReference(int pos) SHARED_REQUIRES(Locks::mutator_lock_);
    mirror::HeapReference<mirror::Object> * GetReferenceAddress(int pos)
            SHARED_REQUIRES(Locks::mutator_lock_);

    // Copied from mirror/object.h
    bool GetStubFlag();

    size_t GetSize();

    void RawDump();

    void SemanticDump() SHARED_REQUIRES(Locks::mutator_lock_);

    int GetNumRefs() { return num_refs_; }

    uint32_t GetForwardingAddress() { return forwarding_address_; }
    void SetForwardingAddress(uint32_t addr) { forwarding_address_ = addr; }

    mirror::Object * GetObjectAddress() {
        return GetTableEntry()->GetObjectAddress();
    }

    void SetObjectAddress(mirror::Object * obj) {
        GetTableEntry()->SetObjectAddress(obj);
    }

    void LockTableEntry();
    void UnlockTableEntry();

    TableEntry * GetTableEntry() {
        return reinterpret_cast<TableEntry *>(table_entry_);
    }

    void SetTableEntry(TableEntry * entry) {
        table_entry_ = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(entry));
    }

  private:
    // Copied from mirror/object.h
    static uint8_t GetBitsAtomic8(const std::atomic<uint8_t> & data, uint8_t offset,
                                  uint8_t width, std::memory_order order);
    static void SetBitsAtomic8(std::atomic<uint8_t> & data, uint8_t offset, uint8_t width,
                               std::memory_order order);
    static void ClearBitsAtomic8(std::atomic<uint8_t> & data, uint8_t offset, uint8_t width,
                                 std::memory_order order);

    static int CountReferences(mirror::Object * object)
            SHARED_REQUIRES(Locks::mutator_lock_);

    void SetStubFlag();
    void ClearFlags();

    // The entry corresponding to the object represented by this stub in the
    // reclamation table.
    uint64_t table_entry_;

    /*
     * x_flags_ layout:
     * 7|6|543210
     * s|
     *
     * s: stub flag
     */
    std::atomic<uint8_t> x_flags_;
    uint8_t padding_b_;
    uint16_t num_refs_;

    uint32_t forwarding_address_;
};

// Used as entry point for compiled code
void PopulateStub(Stub * stub, mirror::Object * object) SHARED_REQUIRES(Locks::mutator_lock_);

} // namespace swap
} // namespace niel
} // namespace art

#endif // ART_RUNTIME_NIEL_STUB_H_
