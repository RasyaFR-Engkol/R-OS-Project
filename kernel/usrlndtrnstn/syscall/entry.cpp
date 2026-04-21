#include <rosval.h>
#include <cpu_context.hpp>
#include <rossys.hpp>
#include "syscall/fs.hpp"
#include "syscall/process.hpp"
#include "syscall/network.hpp"
#include "syscall/mm.hpp"
#define PRINTK_MODULE_NAME "SYSCALL"
#include <logging.hpp>
#include <task.hpp>

#define SYSTABLE(no, func) case no: func(CPUContext); goto end_syscall;

ABI_C VOID Syscall_Entry(CpuContext_T *CPUContext){
    U64 syscall_number = CPUContext->rax;
    if(syscall_number == 1001) {
        Printk::Write(Printk::Level::LOG_DEBUG,
                      "Syscall_Entry: Sys_SetAppPerm called with RDI=0x%llx\n",
                      (unsigned long long)CPUContext->rdi);
    }

    switch(syscall_number){
        #include "table/sys_table.hpp"
        
        default:{
            // Unhandled syscall
            Printk::Write(Printk::Level::LOG_WARNING,
                          "Syscall: Unhandled syscall number %llu\n",
                          (unsigned long long)syscall_number);
            CPUContext->rax = (U64)(-1); // Error
        }
    }

    end_syscall:{
        Tasking::Task *CurTask = Tasking::GetCurrentTaskPtr();
        if(CurTask){
        //Printk::Write(Printk::Level::LOG_INFO,
        //               "Syscall_Entry: Checking signals for PID %llu, Signals=0x%llx\n",
        //               CurTask->pid,
        //               (unsigned long long)CurTask->Signals);
            // Cek Bitmask SIGINT (Bit 2)
        if (CurTask->Signals & (1 << 2)) {

            // Check if Custom Handler exists
            if (CurTask->SignalHandlers[2] != 0) {
                /*Printk::Write(Printk::Level::LOG_DEBUG,
                              "Scheduler: Delivering SIGINT to PID %llu via custom handler at 0x%llx\n",
                              CurTask->pid,
                              (unsigned long long)CurTask->SignalHandlers[2]);*/
                // --- HANDLE SIGNAL (User Mode Handler) ---
                
                // 1. Save Context to User Stack (Red Zone safe)
                // Kita simpan state CPU saat ini ke stack user, supaya nanti bisa di-restore (sigreturn)
                // Asumsi: Stack user valid dan bisa diakses (User CR3 aktif)
                U64 OldRSP = CPUContext->rsp;
                U64 StackFrameSize = sizeof(CpuContext_T);
                U64 NewRSP = (OldRSP - 128 - StackFrameSize) & ~0xF; // Red zone + Align 16

                CpuContext_T* Frame = (CpuContext_T*)NewRSP;
                *Frame = *CPUContext; // Copy struct

                // 2. Setup CPU Context untuk lompat ke Handler
                CPUContext->rip = CurTask->SignalHandlers[2]; // Jump to Handler
                CPUContext->rsp = NewRSP;                     // Switch to new stack
                CPUContext->rdi = 2;                          // Arg1: Signum (System V ABI)

                // Note: Handler harus panggil syscall 'sigreturn' untuk restore context dari stack
                // atau exit() kalau memang tujuannya terminate (seperti ping).
            } else {
                CurTask->vruntime = Tasking::MinVRuntime - 1;

                // Wake parent
                U64 ppid = CurTask->ppid;
                if (ppid < MAX_TASK) {
                    Tasking::Task *parentTask = Tasking::GetTaskPID(ppid);
                    // Parent (Shell) biasanya lagi nungguin (waitpid)
                    if (parentTask != nullptr && parentTask->State == Tasking::TaskState::BLOCKED) {
                        parentTask->State = Tasking::TaskState::READY;
                    }
                }

                Tasking::GraveyardArray[CurTask->pid] = CurTask;
                Tasking::ForceReschedule = TRUE;

                // panggil REAPD biar bersihin
                // nanti REAPD yang free resources-nya
                // kita gak boleh free di syscall context

                Tasking::SchedulerYield(); // Bye bye world
            }
        }
        }
    }

}
