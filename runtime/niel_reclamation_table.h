#ifndef ART_RUNTIME_NIEL_RECLAMATION_TABLE_H_
#define ART_RUNTIME_NIEL_RECLAMATION_TABLE_H_

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace art {

namespace mirror {
    class Object;
}

namespace niel {

namespace swap {

const unsigned int OCCUPIED_BIT_OFFSET = 0;
const unsigned int DIRTY_BIT_OFFSET = 1;
const unsigned int KERNEL_LOCK_BIT_OFFSET = 2;

class TableEntry {
  public:
    bool GetOccupiedBit() {
        return GetBit(OCCUPIED_BIT_OFFSET);
    }

    void SetOccupiedBit() {
        SetBit(OCCUPIED_BIT_OFFSET);
    }

    void ClearOccupiedBit() {
        ClearBit(OCCUPIED_BIT_OFFSET);
    }

    bool GetDirtyBit() {
        return GetBit(DIRTY_BIT_OFFSET);
    }

    void SetDirtyBit() {
        SetBit(DIRTY_BIT_OFFSET);
    }

    void ClearDirtyBit() {
        ClearBit(DIRTY_BIT_OFFSET);
    }

    bool GetKernelLockBit() {
        return GetBit(KERNEL_LOCK_BIT_OFFSET);
    }

    void SetKernelLockBit() {
        SetBit(KERNEL_LOCK_BIT_OFFSET);
    }

    void ClearKernelLockBit() {
        ClearBit(KERNEL_LOCK_BIT_OFFSET);
    }

    uint8_t GetAppLockCounter() {
        return app_lock_counter_.load();
    }

    void IncrAppLockCounter() {
        app_lock_counter_++;
    }

    void DecrAppLockCounter() {
        app_lock_counter_--;
    }

    uint16_t GetNumPages() {
        return num_pages_;
    }

    void SetNumPages(uint16_t num) {
        num_pages_ = num;
    }

    mirror::Object * GetObjectAddress() {
        return reinterpret_cast<mirror::Object *>(object_address_);
    }


    void SetObjectAddress(mirror::Object * obj) {
        object_address_ = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(obj));
    }

    TableEntry() {
        stub_back_pointer_ = 0; // prevents the compiler from complaining about an unused variable
    }

  private:
    bool GetBit(unsigned int offset) {
        return ((bit_flags_.load() >> offset) & 0x1);
    }

    void SetBit(unsigned int offset) {
        bit_flags_.fetch_or(1 << offset);
    }

    void ClearBit(unsigned int offset) {
        bit_flags_.fetch_and(~(1 << offset));
    }

    std::atomic<uint8_t> bit_flags_;
    std::atomic<uint8_t> app_lock_counter_;
    uint16_t num_pages_;
    uint32_t object_address_;
    uint32_t stub_back_pointer_; // only used by compiled code
};

class ReclamationTable {
  public:
    static ReclamationTable CreateTable(int numEntries);
    ReclamationTable() : base_address_(nullptr), num_entries_(0) { }
    TableEntry * CreateEntry();
    void FreeEntry(TableEntry * entry);
    bool IsValid();
    void DebugPrint();

  private:
    ReclamationTable(void * base_address, size_t num_entries)
            : base_address_((TableEntry *)base_address), num_entries_(num_entries) { }
    TableEntry * Begin() {
        return base_address_;
    }
    TableEntry * End() {
        return base_address_ + num_entries_;
    }

    TableEntry * base_address_;
    size_t num_entries_;
};

} // namespace swap
} // namespace niel
} // namespace art

#endif // ART_RUNTIME_NIEL_RECLAMATION_TABLE_H_
