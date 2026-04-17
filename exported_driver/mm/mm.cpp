#include <kernel_api.hpp>

// Gunakan extern "C" supaya namanya tetap "module_init" di dalam ELF
ABI_C int module_init(KernelAPI *API) {
    // Pastikan cara manggil Printk-nya bener sesuai isi struct KernelAPI lo
    if (API && API->Printk) {
        API->Printk(Printk::Level::LOG_INFO, "Hello from the module! This is a test message.\n");
    }
    
    return 0; // 0 = Success
}