#pragma once

#include "spinlock/simple.hpp"
#include "task.hpp"
#include <rosval.h>
#include <rossys.hpp>

// Pastikan typedef Spinlock ada atau include spinlock.hpp
// #include "spinlock.hpp" 

typedef VOID* MutexHandle;

class Mutex {
private:
    // PENJAGA: Ini spinlock buat ngelindungin data internal mutex
    // (queue, locked status, owner) dari race condition.
    Arch::Spinlock::Spinlock m_Lock; 

    BOOL locked;
    Tasking::Task* waitQueueHead; // Pointer ke Task yang ngantri paling depan
    Tasking::Task* waitQueueTail; // Pointer ke Task yang ngantri paling belakang
    Tasking::Task* owner;         // Siapa yang lagi pegang mutex ini?

public:     
    Mutex();

    // Fungsi utama
    VOID Acquire(); // Lock (Tidur kalau gak dapet)
    VOID Release(); // Unlock (Bangunin yang ngantri)

    // Helper
    BOOL IsLocked();
    MutexHandle GetHandle();
};