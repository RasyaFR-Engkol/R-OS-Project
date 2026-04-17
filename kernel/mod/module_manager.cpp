#include <kernel_api.hpp>
#include <../misc/file/extension/elf.hpp>
#include <logging.hpp>

namespace ModuleManager{
    static KernelAPI g_CoreAPI = {
        .Printk = Printk::Write,
        .Panic = Printk::Panic,
        .AllocateDMAPages = PageAlloc::DMAAlloc::AllocateDMAPages,
        .AllocateDMABytes = PageAlloc::DMAAlloc::AllocateDMABytes,
        .FreeDMAPages = PageAlloc::DMAAlloc::FreeDMAPages
    };

    int LoadModuleAndRun(VOID* FileBuffer){
        U64 imageBase, imageEnd;
        
        // Panggil Loader dari Step 1
        U64 entryAddress = ELF::LoadKernelModule(FileBuffer, DoCR3::GetCurrentCR3(), &imageBase, &imageEnd);
        
        if (entryAddress == (U64)ELF_ERR_UNSUPPORTED || entryAddress == 0) {
            Printk::Write(Printk::Level::LOG_ERR, "Gagal nge-load module!\n");
            return -1;
        }

        // 3. THE MAGIC LIVES HERE
        // Ubah alamat mentah jadi pemanggilan fungsi C++
        ModuleInitFunc init_module = reinterpret_cast<ModuleInitFunc>(entryAddress);

        Printk::Write(Printk::Level::LOG_INFO, "Mengeksekusi Module Init...\n");

        // 4. Jalankan modulnya dan lempar g_CoreAPI sebagai parameter
        int status = init_module(&g_CoreAPI);
        
        if (status == 0) {
            Printk::Write(Printk::Level::LOG_INFO, "Module berhasil diinisialisasi!\n");
            // TODO: Lo bisa simpan imageBase dan status ke sebuah List/Vector 
            // supaya nanti gampang kalau mau nge-unload modulnya.
        } else {
            Printk::Write(Printk::Level::LOG_ERR, "Module init return error code!\n");
        }

        return status;
    }
}
