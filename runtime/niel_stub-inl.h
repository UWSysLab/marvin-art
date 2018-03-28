#ifndef ART_RUNTIME_NIEL_STUB_INL_H_
#define ART_RUNTIME_NIEL_STUB_INL_H_

#include "niel_stub.h"

namespace art {

namespace niel {

namespace swap {

inline mirror::HeapReference<mirror::Object> * Stub::GetReferenceAddress(int pos) {
    char * refBytePtr = (char *)this + sizeof(Stub)
                                     + pos * sizeof(mirror::HeapReference<mirror::Object>);
    return (mirror::HeapReference<mirror::Object> *)refBytePtr;
}

void Stub::SetReference(int pos, mirror::Object * ref) {
    GetReferenceAddress(pos)->Assign(ref);
}

mirror::Object * Stub::GetReference(int pos) {
    return GetReferenceAddress(pos)->AsMirrorPtr();
}

// Copied from mirror/object.h
uint8_t Stub::GetBitsAtomic8(const std::atomic<uint8_t> & data, uint8_t offset,
                            uint8_t width, std::memory_order order) {
    return (data.load(order) >> offset) & (0xff >> (8 - width));
}
void Stub::SetBitsAtomic8(std::atomic<uint8_t> & data, uint8_t offset, uint8_t width,
                         std::memory_order order) {
    data.fetch_or((0xff >> (8 - width) << offset), order);
}
void ClearBitsAtomic8(std::atomic<uint8_t> & data, uint8_t offset, uint8_t width,
                           std::memory_order order) {
    data.fetch_and(~((0xff >> (8 - width)) << offset), order);
}


} // namespace swap
} // namespace niel
} // namespace art

#endif // ART_RUNTIME_NIEL_STUB_INL_H_
