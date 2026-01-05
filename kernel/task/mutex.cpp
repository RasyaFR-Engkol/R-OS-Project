#include <spinlock/mutex.hpp>
#include "rosval.h"
#include "task.hpp"
#include <spinlock/simple.hpp>

Mutex::Mutex() {
    // Inisialisasi awal
    m_Lock.Init(); // Reset spinlock
    locked = FALSE;
    owner = nullptr;
    waitQueueHead = nullptr;
    waitQueueTail = nullptr;
}

VOID Mutex::Acquire(){
    m_Lock.Acquire();

    Tasking::Task *Me = Tasking::GetCurrentTaskPtr();

    if(locked == false){
        locked = true;
        owner = Me;
        m_Lock.Release();
    } else {
        Me->NextWaitTask = nullptr;

        if(waitQueueHead == nullptr){
            waitQueueHead = Me;
            waitQueueTail = Me;
        } else {
            waitQueueTail->NextWaitTask = Me;
            waitQueueTail = Me;
        }   

        Me->State = Tasking::TaskState::BLOCKED;

        m_Lock.Release();

        Tasking::SchedulerYield();
    }
}

VOID Mutex::Release(){
    m_Lock.Acquire();

    if(waitQueueHead != nullptr){
        Tasking::Task *NextTask = waitQueueHead;

        waitQueueHead = waitQueueHead->NextWaitTask;

        if(waitQueueHead == nullptr){
            waitQueueTail = nullptr;
        }

        owner = NextTask;

        Tasking::UnblockTaskWithIOBoost(NextTask);
    } else {
        locked = false;
        owner = nullptr;
    }

    m_Lock.Release();
}

BOOL Mutex::IsLocked(){
    m_Lock.Acquire();
    BOOL status = locked;
    m_Lock.Release();
    return status;
}

MutexHandle Mutex::GetHandle(){
    return (MutexHandle)this;
}