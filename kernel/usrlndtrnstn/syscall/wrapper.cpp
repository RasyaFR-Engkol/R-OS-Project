#include <cpu_context.hpp>
#include <serial.hpp>

extern "C" VOID Syscall_Entry(CpuContext_T *CPUContext);
extern "C" VOID Context_Restore(VOID *Context);

extern "C" void Syscall_Wrapper(void *ctx) {
    //Serial::Printf("[ROS] Syscall_Wrapper: entered syscall wrapper\n");
    // Call the C syscall handler which updates CpuContext (rax for return)
    Syscall_Entry((CpuContext_T*)ctx);
    //Serial::Printf("[ROS] Syscall_Wrapper: ret context\n");
    // Now perform the context restore (will iretq back to user)
    Context_Restore(ctx);
}
