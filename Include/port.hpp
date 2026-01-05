#pragma once

// Untuk PORT PORT
#include "rosval.h"
#include "../kernel/driver/pci/pci.hpp"

namespace Port {
    // Port I/O helpers (freestanding, static inline)
    // Supports 8-bit, 16-bit and 32-bit I/O which are the sizes
    // supported by x86/x86_64 'in'/'out' instructions.

    // Read 8-bit from I/O port
    static inline U8 Inb(U16 port) {
        U8 ret;
        asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
        return ret;
    }

    // Write 8-bit to I/O port
    static inline void Outb(U16 port, U8 value) {
        asm volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
    }

    // Read 16-bit from I/O port
    static inline U16 Inw(U16 port) {
        U16 ret;
        asm volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
        return ret;
    }

    // Write 16-bit to I/O port
    static inline void Outw(U16 port, U16 value) {
        asm volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
    }

    // Read 32-bit from I/O port
    static inline U32 Inl(U16 port) {
        U32 ret;
        asm volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
        return ret;
    }

    // Write 32-bit to I/O port
    static inline void Outl(U16 port, U32 value) {
        asm volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
    }

    // Aliases matching some naming conventions (optional)
    static inline U32 Ind(U16 port) { return Inl(port); }
    static inline void Outd(U16 port, U32 v) { Outl(port, v); }

    // 64-bit port I/O is not supported by x86 'in'/'out' instructions.
    // If you need wider transfers use multiple 32-bit ops or memory-mapped I/O.
}

// MMIO helpers: read/write memory-mapped I/O registers. These operate on
// virtual addresses (so the caller must pass a virtual address that is
// already mapped, e.g. via HHDM or ioremap). Accesses are volatile to
// prevent compiler reordering; we also emit a compiler memory barrier after
// writes to avoid reordering across the write.
namespace MMIOPort {
    static inline U8 Read8(UPTR addr) {
        return *(volatile U8*)addr;
    }
    static inline void Write8(UPTR addr, U8 value) {
        *(volatile U8*)addr = value;
        asm volatile ("" ::: "memory");
    }

    static inline U16 Read16(UPTR addr) {
        return *(volatile U16*)addr;
    }
    static inline void Write16(UPTR addr, U16 value) {
        *(volatile U16*)addr = value;
        asm volatile ("" ::: "memory");
    }

    static inline U32 Read32(UPTR addr) {
        return *(volatile U32*)addr;
    }
    static inline void Write32(UPTR addr, U32 value) {
        *(volatile U32*)addr = value;
        asm volatile ("" ::: "memory");
    }

    static inline U64 Read64(UPTR addr) {
        return *(volatile U64*)addr;
    }
    static inline void Write64(UPTR addr, U64 value) {
        *(volatile U64*)addr = value;
        asm volatile ("" ::: "memory");
    }
}