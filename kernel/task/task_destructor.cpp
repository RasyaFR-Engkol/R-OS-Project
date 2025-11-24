#define PRINTK_MODULE_NAME "TaskDestructor"
#include <rossys.hpp>
#include <rosval.h>
#include <logging.hpp>
#include "task.hpp"
#include "../mm/mm.hpp"
#include "../mm/kmalloc/kmalloc.hpp"
#include <filesystem/filesystem.hpp>
#include "../../kernel/driver/pic/pic.hpp"

namespace Tasking {

    VOID FreeUserAddressSpace(U64 cr3) {
        if (cr3 == 0) return;

        U64 *pml4 = HHDM_PhysToVirt(cr3);

        // Iterate over user space entries (0 to 255)
        // Kernel space (256-511) is shared and should not be freed
        for (int i = 0; i < 256; ++i) {
            if (pml4[i] & PAGE_PRESENT) {
                U64 pdptPhys = pml4[i] & PAGE_ADDR_MASK;
                U64 *pdpt = HHDM_PhysToVirt(pdptPhys);

                for (int j = 0; j < 512; ++j) {
                    if (pdpt[j] & PAGE_PRESENT) {
                        // Check for 1GB huge page
                        if (pdpt[j] & PAGE_PS) {
                            // We don't expect 1GB user pages usually
                            // If we did, we would free the 1GB physical block here
                            Printk::Write(Printk::Level::LOG_EMERG, "Address Space Corruption : encountered 1GB huge page, which is unexpected\n");
                            continue; 
                        }

                        U64 pdPhys = pdpt[j] & PAGE_ADDR_MASK;
                        U64 *pd = HHDM_PhysToVirt(pdPhys);

                        for (int k = 0; k < 512; ++k) {
                            if (pd[k] & PAGE_PRESENT) {
                                // Check for 2MB huge page
                                if (pd[k] & PAGE_PS) {
                                    // Mask bits 12-20 for 2MB page alignment (and PAT bit)
                                    U64 phys = (pd[k] & PAGE_ADDR_MASK) & ~0x1FF000ULL;
                                    PageAlloc::PhysicalFreePages(phys, 512); // 2MB = 512 pages
                                } else {
                                    U64 ptPhys = pd[k] & PAGE_ADDR_MASK;
                                    U64 *pt = HHDM_PhysToVirt(ptPhys);

                                    for (int l = 0; l < 512; ++l) {
                                        if (pt[l] & PAGE_PRESENT) {
                                            U64 pagePhys = pt[l] & PAGE_ADDR_MASK;
                                            PageAlloc::PhysicalFreePages(pagePhys, 1);
                                        }
                                    }
                                    PageAlloc::PhysicalFreePages(ptPhys, 1);
                                }
                            }
                        }
                        PageAlloc::PhysicalFreePages(pdPhys, 1);
                    }
                }
                PageAlloc::PhysicalFreePages(pdptPhys, 1);
            }
        }
        
        // Finally free the PML4 itself
        PageAlloc::PhysicalFreePages(cr3, 1);
    }

    VOID DestroyTask(Task *task) {
        if (!task) return;

        // Prevent scheduler from switching to this task while we destroy it
        // Ideally we should hold a lock here
        LOCKRFLAGS irq = Arch::SaveAndDisableInterrupts();

        // 1. Remove from scheduler
        for (int i = 0; i < MAX_TASK; ++i) {
            if (TaskArray[i] == task) {
                TaskArray[i] = nullptr;
                if (ActiveTask > 0) ActiveTask--;
                // If this was the active task, we can't just null it out and continue running
                // The caller (scheduler or exit syscall) must handle the context switch
                break;
            }
        }

        Arch::RestoreInterrupts(irq);

        // 2. Close file descriptors
        for (VAL32 i = 0; i < MAX_FILE_IN_PROCESS; ++i) {
            if (task->FDTable[i]) {
                task->FDTable[i]->RefCount--;
                if (task->FDTable[i]->RefCount <= 0) {
                    VFSManager::Close(task->FDTable[i]);
                }
                task->FDTable[i] = nullptr;
            }
        }

        // 3. Free Address Space (if user task)
        // Check if it's not the kernel page table
        if (task->CR3 != (U64)KernelPML4Phys) {
            FreeUserAddressSpace(task->CR3);
        }

        // 4. Free Kernel Stack
        if (task->StackBase) {
            Kmalloc::Free(task->StackBase);
        }

        PIC::Keyboard::NotifyTaskDied(task);

        // 5. Free Task Struct
        delete task;
    }
}
