#include <kernel_api.hpp>

// Gunakan extern "C" supaya namanya tetap "module_init" di dalam ELF
ABI_C int module_init(VOID *PrivateData) {
    // Pastikan cara manggil Printk-nya bener sesuai isi struct KernelAPI lo
    PrintkWrite(Printk::Level::LOG_INFO, "Hello from mm module!\n");
    PrintkWrite(Printk::Level::LOG_INFO, "KernelAPI version: %d\n", 1);
    return 0; // 0 = Success
}