#include <rosval.h>
#include <cpu_context.hpp>
#include <rossys.hpp>
#include "syscall/fs.hpp"
#include "syscall/process.hpp"
#define PRINTK_MODULE_NAME "SYSCALL"
#include <logging.hpp>
#include <task.hpp>

#define SYSTABLE(no, func) case no: func(CPUContext); break;

ABI_C VOID Syscall_Entry(CpuContext_T *CPUContext){
    U64 syscall_number = CPUContext->rax;
    //Serial::Printf("[ROS] Syscall_Entry: syscall number %llu\n", (unsigned long long)syscall_number);

    switch(syscall_number){
        #include "table/sys_table.hpp"
        
        default:
            // Unhandled syscall
            Printk::Write(Printk::Level::LOG_WARNING,
                          "Syscall: Unhandled syscall number %llu\n",
                          (unsigned long long)syscall_number);
            CPUContext->rax = (U64)(-1); // Error
            break;
    }
}
