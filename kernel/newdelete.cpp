// Simple global operator new/delete implementations for kernel use.
// Uses the kernel kmalloc allocator.

#include "mm/kmalloc/kmalloc.hpp"
#include "serial.hpp"
// Don't include the C++ <new> header (standard library headers may not be
// available in this kernel build). Forward-declare the nothrow type so we
// can provide the nothrow overloads' signatures.
namespace std { struct nothrow_t; }

// Basic single-object new/delete
void* operator new(size_t size) {
    return Kmalloc::Alloc((SIZE_T)size);
}

void operator delete(void* ptr) noexcept {
    if (ptr) Kmalloc::Free(ptr);
}

// Array new/delete
void* operator new[](size_t size) {
    return Kmalloc::Alloc((SIZE_T)size);
}

void operator delete[](void* ptr) noexcept {
    if (ptr) Kmalloc::Free(ptr);
}

// Nothrow variants
void* operator new(size_t size, const std::nothrow_t&) noexcept {
    return Kmalloc::Alloc((SIZE_T)size);
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    return Kmalloc::Alloc((SIZE_T)size);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    if (ptr) Kmalloc::Free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    if (ptr) Kmalloc::Free(ptr);
}

// Sized delete (C++14+)
void operator delete(void* ptr, size_t) noexcept {
    if (ptr) Kmalloc::Free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {
    if (ptr) Kmalloc::Free(ptr);
}
