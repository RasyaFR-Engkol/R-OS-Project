#pragma once

#include "rosval.h"
#include <task.hpp>

#define MAX_IRQS 255

enum IrqReturn{
    IRQ_NONE = 0,
    IRQ_HANDLED = 1,
    IRQ_WAKE_THREAD = 2
};

struct IrqAction{
    VOID (*Handler)(VOID* Ctx1);
    VOID (*ThreadFunc)(void *data);
    VOID *DevID;

    Tasking::Task *WorkerThread;
    Tasking::WaitQueue Queue;

    VOLATILE BOOL PendingWorkRn;
};

ABI_C {
    VOID RequestThreadedIrq(U8 Vector, VOID (*TopHalf)(VOID* Ctx1), VOID (*BottomHalf)(VOID*), VOID *DevID);
    VOID WakeUpThreadedIrq(U8 Vector);
}