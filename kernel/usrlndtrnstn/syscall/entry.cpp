#include <rosval.h>
#include <cpu_context.hpp>
#include <rossys.hpp>
#include "syscall/fs.hpp"
#include "syscall/process.hpp"
#define PRINTK_MODULE_NAME "SYSCALL"
#include <logging.hpp>
#include <task.hpp>

#define SYSTABLE(no, func) case no: func(CPUContext); goto end_syscall;

ABI_C VOID Syscall_Entry(CpuContext_T *CPUContext){
    U64 syscall_number = CPUContext->rax;
    //Serial::Printf("[ROS] Syscall_Entry: syscall number %llu\n", (unsigned long long)syscall_number);

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
            
            //Printk::Write(Printk::Level::LOG_INFO, "Syscall_Entry: Delivering SIGINT to PID %llu\n", CurTask->pid);
            
            CurTask->Signals &= ~(1 << 2); // Clear SIGINT bit
            CurTask->State = Tasking::TaskState::ZOMBIE;

            // Wake parent
            U64 ppid = CurTask->ppid;
            if (ppid < MAX_TASK) {
                Tasking::Task *parentTask = Tasking::GetTaskPID(ppid);
                // Parent (Shell) biasanya lagi nungguin (waitpid)
                if (parentTask != nullptr && parentTask->State == Tasking::TaskState::BLOCKED) {
                    parentTask->State = Tasking::TaskState::READY;
                }
            }

                Tasking::SchedulerYield(); // Bye bye world
            }
        }
    }

}
