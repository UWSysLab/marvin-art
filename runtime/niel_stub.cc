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
    forwarding_address_ = 0;
    padding_a_ = 0;
    padding_b_ = 0;
    padding_c_ = 0;

    num_refs_ = CountReferences(object);

    for (int i = 0; i < num_refs_; i++) {
        GetReferenceAddress(i)->Assign(nullptr);
    }

    StubPopulateVisitor visitor(this);
    DummyReferenceVisitor dummyVisitor;
    object->VisitReferences(visitor, dummyVisitor);
}

void Stub::RawDump() {
    LOG(INFO) << "NIEL raw dump for stub @" << this;
    size_t stubSize = GetSize();
    char * stubData = (char *)this;
    for (size_t i = 0; i < stubSize; i++) {
        LOG(INFO) << i << ": " << std::hex << (int)stubData[i];
    }
    LOG(INFO) << "NIEL end raw dump for stub @" << this;
}

void Stub::SemanticDump() {
    LOG(INFO) << "NIEL semantic dump for stub @" << this;
    LOG(INFO) << "forwarding_address_: " << std::hex << forwarding_address_;
    LOG(INFO) << "stub flag: "<< GetStubFlag();
    LOG(INFO) << "num_refs_: " << num_refs_;
    for (int i = 0; i < GetNumRefs(); i++) {
        std::string refString;
        mirror::Object * ref = GetReference(i);
        if (ref == nullptr) {
            refString = "null";
        }
        else {
            if (ref->GetStubFlag()) {
                refString = "stub";
            }
            else {
                mirror::Class * klass = ref->GetClass();
                if (klass == nullptr) {
                    refString = "null class";
                }
                refString = PrettyClass(klass);
            }
        }
        LOG(INFO) << "ref " << i << ": " << ref << " " << refString;
    }
    LOG(INFO) << "NIEL end semantic dump for stub @" << this;
}

} // namespace swap
} // namespace niel
} // namespace art
