#include "string.hpp"
#include <kernel_api.hpp>
#include <../misc/file/extension/elf.hpp>
#include <logging.hpp>
#include <mm.hpp>
#include <export_sym.hpp>

namespace ModuleManager{
    UPTR FindKernelSymbol(const char* name){
        KernelSymbol *current = __ksymtab_start;
        while (current < __ksymtab_end){
            if (String::Strcmp(current->Name, name) == 0){
                return current->Value;
            }
            current++;
        }
        return 0; // Not found
    }

    int LoadModuleAndRun(VOID* FileBuffer, VOID *PrivateData){
        U64 imageBase, imageEnd;
        
        // Panggil Loader dari Step 1
        U64 entryAddress = ELF::LoadKernelModule(FileBuffer, HHDM_PhysToVirt((UPTR)DoCR3::GetCurrentCR3()), &imageBase, &imageEnd);
        
        if (entryAddress == (U64)ELF_ERR_UNSUPPORTED || entryAddress == 0) {
            Printk::Write(Printk::Level::LOG_ERR, "Gagal nge-load module!\n");
            return -1;
        }

        // 3. THE MAGIC LIVES HERE
        // Ubah alamat mentah jadi pemanggilan fungsi C++
        ModuleInitFunc init_module = reinterpret_cast<ModuleInitFunc>(entryAddress);

        // 4. Jalankan modulnya dan lempar g_CoreAPI sebagai parameter
        int status = init_module(PrivateData);
        
        if(status < 0){
            Printk::Write(Printk::Level::LOG_ERR, "Module returning error when inited (code: %d)\n", status);
        }

        return status;
    }
}

