#pragma once

#include "rosval.h"
typedef volatile int SPINLOCK_T;

namespace Arch{
    namespace Spinlock{
        // C++ wrapper around the raw SPINLOCK_T value. Provided as an
        // ergonomic alternative to the C-style API below. The existing
        // C-style functions are kept for compatibility.
        class Spinlock {
        private:
            SPINLOCK_T _v;

        public:
            constexpr Spinlock() : _v(0) {}

            // initialize to unlocked
            inline void Init() { _v = 0; }

            // acquire the lock (busy-wait)
            inline void Acquire() {
                VAL32 One = 1;
                VAL32 OldVal;
                __asm__ volatile(
                    "xchgl %0, %1"
                    : "=r"(OldVal), "+m"(_v)
                    : "0"(One)
                    : "memory"
                );

                while (OldVal != 0) {
                    // spin until we get the lock
                    __asm__ volatile("pause");
                    __asm__ volatile(
                        "xchgl %0, %1"
                        : "=r"(OldVal), "+m"(_v)
                        : "0"(One)
                        : "memory"
                    );
                }
            }

            // release the lock
            inline void Release() {
                VAL32 Zero = 0;
                __asm__ volatile(
                    "xchgl %0, %1"
                    : "=r"(Zero), "+m"(_v)
                    : "0"(Zero)
                    : "memory"
                );
            }

            // Access raw pointer if caller needs C-style API interop
            inline SPINLOCK_T* Raw() { return &_v; }
        };

        // RAII guard: acquires in ctor, releases in dtor. Prevents forgetting
        // to release a lock in a function with multiple returns.
        class SpinlockGuard {
        private:
            Spinlock &L;
            bool owned;
        public:
            explicit SpinlockGuard(Spinlock &l) : L(l), owned(true) { L.Acquire(); }
            ~SpinlockGuard() { if (owned) L.Release(); }
            // non-copyable
            SpinlockGuard(const SpinlockGuard&) = delete;
            SpinlockGuard& operator=(const SpinlockGuard&) = delete;
            // movable
            SpinlockGuard(SpinlockGuard&& o) : L(o.L), owned(o.owned) { o.owned = false; }
        };

        static inline VOID SpinlockInit(SPINLOCK_T *Lock){
            *Lock = 0;
        }

        static inline VOID SpinLockAcquire(SPINLOCK_T *Lock){
            while(1){
                VAL32 One = 1;
                VAL32 OldVal;

                __asm__ volatile(
                    "xchgl %0, %1"
                    : "=r"(OldVal), "+m"(*Lock)
                    : "0"(One)
                    : "memory"
                );

                if(OldVal == 0){
                    break;
                }

                __asm__ volatile("pause");
            }
        }

        static inline VOID SpinLockRelease(SPINLOCK_T *Lock){
            VAL32 Zero = 0;
            __asm__ volatile(
                "xchgl %0, %1"
                : "=r"(Zero), "+m"(*Lock)
                : "0"(Zero)
                : "memory"
            );
        }
    }
}