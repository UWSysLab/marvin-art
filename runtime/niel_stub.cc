#include "niel_stub.h"

#include <iostream>

#include "mirror/object.h"
#include "mirror/object_reference.h"
#include "niel_stub-inl.h"

namespace art {

namespace niel {

namespace swap {

void Stub::Init(int numRefs) {
    num_refs_ = numRefs;
    ClearFlags();
    SetStubFlag();

    padding_a_ = 0;
    padding_b_ = 0;
    padding_c_ = 0;
    padding_d_ = 0;

    for (int i = 0; i < numRefs; i++) {
        GetReferenceAddress(i)->Assign(nullptr);
    }
}

void Stub::Dump() {
    LOG(INFO) << "NIEL stub dump:";
    size_t stubSize = GetStubSize(num_refs_);
    char * stubData = (char *)this;
    for (size_t i = 0; i < stubSize; i++) {
        LOG(INFO) << i << ": " << std::hex << (int)stubData[i];
    }
    LOG(INFO) << "NIEL end stub dump.";
}

} // namespace swap
} // namespace niel
} // namespace art
