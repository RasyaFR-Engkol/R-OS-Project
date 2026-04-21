#include "export_sym.hpp"
#include <rosval.h>
#include <rossys.hpp>
#define PRINTK_MODULE_NAME "ThreadedIRQ"
#include <mm.hpp>
#include "threaded_irq.hpp"
#include <logging.hpp>
#include <task.hpp>

static IrqAction *RegisteredIrqActions[MAX_IRQS] = { nullptr };

void IrqWorkerWrapper(void* arg) {
    IrqAction* action = (IrqAction*)arg;

    while (true) {
        Arch::ASM::Cli(); 

        if (!action->PendingWorkRn) {
            Arch::ASM::Sti(); 
            // JANGAN YIELD! Suruh dia TIDUR MATI di WaitQueue abstraction kita.
            // Dengan gini, dia bakal dicabut dari RunQueue dan CPU lu aman.
            Tasking::SleepOn(action->Queue); 
            continue;
        }
        
        action->PendingWorkRn = false;
        Arch::ASM::Sti(); 

        // Eksekusi Bottom Half driver (xHCI_Worker_Thread)
        if (action->ThreadFunc) {
            action->ThreadFunc(action->DevID);
        }
    }
}

ABI_C {
    VOID WakeUpThreadedIrq(U8 Vector) {
        IrqAction *action = RegisteredIrqActions[Vector];
        if (action) {
            action->PendingWorkRn = true;        // Set flag biar worker mau kerja
            Tasking::WakeUp(action->Queue);      // Bangunin worker dari tidurnya!
        }
    }
    EXPORT_SYMBOL(WakeUpThreadedIrq);

    VOID RequestThreadedIrq(U8 Vector, VOID (*TopHalf)(VOID* Ctx1), VOID (*BottomHalf)(VOID*), VOID *DevID)
    {
        IrqAction *Action = new IrqAction;
        if (RosUnlikely(!Action)) {
            Printk::Write(Printk::Level::LOG_ERR, "Failed to allocate IrqAction for IRQ %u\n", (unsigned)Vector);
            return;
        }
        
        Action->Handler = TopHalf;
        Action->ThreadFunc = BottomHalf;
        Action->DevID = DevID;
        Action->PendingWorkRn = FALSE;

        Action->WorkerThread = Tasking::CreateKThread(IrqWorkerWrapper, Action, "IRQWorker");
        Action->WorkerThread->IsCriticalProc = TRUE; // Tandai sebagai kritikal agar gak di-kill sembarangan
        Action->WorkerThread->vruntime = Tasking::MinVRuntime - 1;

        if (Vector < (U8)MAX_IRQS) {
            RegisteredIrqActions[Vector] = Action;
            Printk::Write(Printk::Level::LOG_INFO, "Registered threaded IRQ handler for vector %u\n", (unsigned)Vector);
        } else {
            Printk::Write(Printk::Level::LOG_ERR, "Invalid IRQ vector %u\n", (unsigned)Vector);
            delete Action;
        }
    }
    EXPORT_SYMBOL(RequestThreadedIrq);
}