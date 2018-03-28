#include "niel_stub.h"

#include <iostream>

#include "mirror/class.h"
#include "mirror/object-inl.h"
#include "mirror/object_reference.h"
#include "niel_stub-inl.h"

namespace art {

namespace niel {

namespace swap {

// Based on MarkVisitor in runtime/gc/collector/mark_sweep.cc
//
// TODO: Make sure none of these things cause correctness issues:
//     1) Using a mutable variable modified from a const function to track the current offset
//     2) Assuming Object::VisitReferences() visits references in a deterministic order
//     3) Assuming the object is not modified during visiting
class StubPopulateVisitor {
  public:
    StubPopulateVisitor(Stub * stub) : stub_(stub), cur_ref_(0) {}

    void operator()(mirror::Object * obj,
                    MemberOffset offset,
                    bool is_static ATTRIBUTE_UNUSED) const
            SHARED_REQUIRES(Locks::mutator_lock_) {
        stub_->SetReference(cur_ref_, obj->GetFieldObject<mirror::Object>(offset));
        cur_ref_++;
    }

    void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED) const {}

    void VisitRoot(mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED) const {}

  private:
    Stub * stub_;
    mutable uint32_t cur_ref_;
};

// Method signature from MarkSweep::DelayReferenceReferentVisitor in
// runtime/gc/collector/mark_sweep.cc.
//
// TODO: Make sure this class is not actually being used when populating stubs
class DummyReferenceVisitor {
  public:
    void operator()(mirror::Class* klass ATTRIBUTE_UNUSED,
                    mirror::Reference* ref ATTRIBUTE_UNUSED) const {}
};

void Stub::Populate(mirror::Object * object) {
    ClearFlags();
    SetStubFlag();
    padding_a_ = 0;
    padding_b_ = 0;
    padding_c_ = 0;
    padding_d_ = 0;

    num_refs_ = CountReferences(object);

    for (int i = 0; i < num_refs_; i++) {
        GetReferenceAddress(i)->Assign(nullptr);
    }

    StubPopulateVisitor visitor(this);
    DummyReferenceVisitor dummyVisitor;
    object->VisitReferences(visitor, dummyVisitor);
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
