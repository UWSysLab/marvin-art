#include "niel_reclamation_table.h"

#include "base/logging.h"

#include <sys/mman.h>

namespace art {

namespace niel {

namespace swap {

ReclamationTable ReclamationTable::CreateTable(int numEntries) {
    size_t size = numEntries * sizeof(TableEntry);
    void * baseAddress = mmap(nullptr,
                              size,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS,
                              -1,
                              0);
    if (baseAddress == MAP_FAILED) {
        LOG(ERROR) << "NIELERROR mmap failed during reclamation table creation: "
                   << strerror(errno);
        return ReclamationTable(nullptr, 0);
    }
    return ReclamationTable(baseAddress, numEntries);
}

TableEntry * ReclamationTable::CreateEntry() {
    TableEntry * curEntry = Begin();
    while (curEntry < End()) {
        if (!curEntry->GetOccupiedBit()) {
            curEntry->SetOccupiedBit();
            return curEntry;
        }
        curEntry++;
    }
    return nullptr;
}

void ReclamationTable::FreeEntry(TableEntry * entry) {
    entry->ClearOccupiedBit();
}

bool ReclamationTable::IsValid() {
    return base_address_ != nullptr;
}

void ReclamationTable::DebugPrint() {
    LOG(INFO) << "NIELDEBUG ReclamationTable base_address_" << base_address_
              << " num_entries_ " << num_entries_;
    for (TableEntry * curEntry = Begin(); curEntry < End(); curEntry++) {
        LOG(INFO) << "NIELDEBUG " << curEntry
                  << "|" << curEntry->GetOccupiedBit()
                  << "|" << curEntry->GetDirtyBit()
                  << "|" << curEntry->GetKernelLockBit()
                  << "|" << (int)curEntry->GetAppLockCounter()
                  << "|" << curEntry->GetNumPages()
                  << "|" << (void *)curEntry->GetObjectAddress();
    }
}

} // namespace swap

} // namespace niel

} // namespace art
