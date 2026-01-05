#include "mm.hpp"
extern "C" void Paging_Initialize_C() {
    Paging::Initialize();
}
extern "C" void Paging_RelocateGDT_C() {
    Paging::RelocateGDTToHigh();
}
extern "C" void Paging_SwitchStack_C() {
    // Switch to new kernel stack and finalize (TSS init + disable low-half)
    Paging::SwitchToKernelStack(8);
}
