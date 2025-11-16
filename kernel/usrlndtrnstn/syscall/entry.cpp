#include <rosval.h>
#include <cpu_context.hpp>
#include <rossys.hpp>
#include "syscall/fs.hpp"

#define SYSTABLE(no, func) case no: func(CPUContext); break;

ABI_C VOID Syscall_Entry(CpuContext_T *CPUContext){
    U64 syscall_number = CPUContext->rax;

    switch(syscall_number){
        #include "table/sys_table.hpp"
        default:
            // Unhandled syscall
            CPUContext->rax = (U64)(-1); // Error
            break;
    }
}
