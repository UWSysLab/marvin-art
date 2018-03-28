#ifndef ART_RUNTIME_NIEL_STUB_INL_H_
#define ART_RUNTIME_NIEL_STUB_INL_H_

#include "niel_stub.h"

namespace art {

namespace niel {

namespace swap {

inline size_t Stub::GetStubSize(int numRefs) {
    return sizeof(Stub) + numRefs * sizeof(mirror::HeapReference<mirror::Object>);
}

inline size_t Stub::GetStubSize(mirror::Object * object) {
    int numRefs = CountReferences(object);
    return GetStubSize(numRefs);
}

inline mirror::HeapReference<mirror::Object> * Stub::GetReferenceAddress(int pos) {
    char * refBytePtr = (char *)this + sizeof(Stub)
                                     + pos * sizeof(mirror::HeapReference<mirror::Object>);
    return (mirror::HeapReference<mirror::Object> *)refBytePtr;
}

inline void Stub::SetReference(int pos, mirror::Object * ref) {
    GetReferenceAddress(pos)->Assign(ref);
}

inline mirror::Object * Stub::GetReference(int pos) {
    return GetReferenceAddress(pos)->AsMirrorPtr();
}

inline int Stub::CountReferences(mirror::Object * object) {
    // Copied from niel_instrumentation.cc
    object->SetIgnoreReadFlag();
    mirror::Class * klass = object->GetClass();
    object->ClearIgnoreReadFlag();
    uint32_t numPointers = klass->NumReferenceInstanceFields();
    mirror::Class * superClass = klass->GetSuperClass();
    while (superClass != nullptr) {
        numPointers += superClass->NumReferenceInstanceFields();
        superClass = superClass->GetSuperClass();
    }
    return numPointers;
}

// Copied from mirror/object.h
inline bool Stub::GetStubFlag() {
  return (bool)GetBitsAtomic8(x_flags_, 7, 1, std::memory_order_acquire);
}

// Copied from mirror/object.h
inline uint8_t Stub::GetBitsAtomic8(const std::atomic<uint8_t> & data, uint8_t offset,
                            uint8_t width, std::memory_order order) {
    return (data.load(order) >> offset) & (0xff >> (8 - width));
}
inline void Stub::SetBitsAtomic8(std::atomic<uint8_t> & data, uint8_t offset, uint8_t width,
                         std::memory_order order) {
    data.fetch_or((0xff >> (8 - width) << offset), order);
}
inline void Stub::ClearBitsAtomic8(std::atomic<uint8_t> & data, uint8_t offset, uint8_t width,
                           std::memory_order order) {
    data.fetch_and(~((0xff >> (8 - width)) << offset), order);
}

inline void Stub::SetStubFlag() {
  SetBitsAtomic8(x_flags_, 7, 1, std::memory_order_acq_rel);
}

inline void Stub::ClearFlags() {
  ClearBitsAtomic8(x_flags_, 0, 8, std::memory_order_acq_rel);
}


} // namespace swap
} // namespace niel
} // namespace art

#endif // ART_RUNTIME_NIEL_STUB_INL_H_
