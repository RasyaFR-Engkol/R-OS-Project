#include "firmware/acpi/acpi.hpp"
#include <rosval.h>
#include <stddef.h>
#include <stdint.h> // Untuk 'uint64_t'
#include <bootinfo.h>
#include <../firmware/chipset/chipset.hpp>
#include <../kernel/dev/devicemanager.hpp>
#include <../firmware/acpi/driver/timer/timer.hpp>
#include "kernel/driver/pic/timer/pit.hpp"
#include "kernel/intidt/idt.hpp"
#include <task.hpp>

#define MB_TAG_TYPE_END 0U
#define MB_TAG_TYPE_FRAMEBUFFER 8U
#define MB_TAG_TYPE_MMAP 6U
#define MB_TAG_TYPE_ACPI_OLD 14U
#define MB_TAG_TYPE_ACPI_NEW 15U
#define MB_ALIGN 8U

static BootInfo gBootInfo;

struct ROS_PACKED multiboot_tag {
    U32 type;
    U32 size;
};

struct ROS_PACKED multiboot_tag_framebuffer {
    U32 type;
    U32 size;
    U64 framebuffer_addr;
    U32 framebuffer_pitch;
    U32 framebuffer_width;
    U32 framebuffer_height;
    U8 framebuffer_bpp;
    U8 framebuffer_type;
    U16 reserved;
    union {
        struct {
            U16 framebuffer_palette_num_colors;
            /* Optional palette entries follow */
        } palette;
        struct {
            U8 framebuffer_red_field_position;
            U8 framebuffer_red_mask_size;
            U8 framebuffer_green_field_position;
            U8 framebuffer_green_mask_size;
            U8 framebuffer_blue_field_position;
            U8 framebuffer_blue_mask_size;
        } rgb;
    } color_info;
};

struct ROS_PACKED multiboot_tag_mmap {
    U32 type;
    U32 size;
    U32 entry_size;
    U32 entry_version;
};

struct ROS_PACKED multiboot_mmap_entry {
    U64 addr;
    U64 len;
    U32 type;
    U32 zero;
};

static inline size_t align_up(size_t value, size_t align) {
    return (value + (align - 1)) & ~(align - 1);
}

static void parse_multiboot2_info(uint64_t mb_info_ptr) {
    BootInfo info = {};
    info.multiboot_addr = mb_info_ptr;

    if (mb_info_ptr == 0) {
        gBootInfo = info;
        return;
    }

    const uint8_t *base = (const uint8_t *)(uintptr_t)mb_info_ptr;
    if (!base) {
        gBootInfo = info;
        return;
    }

     /* Read the 32-bit little-endian total size from the byte buffer without
         performing an aligned pointer cast. This avoids -Wcast-align and is
         safe on architectures that disallow unaligned accesses. */
     uint32_t total_size = (uint32_t)base[0]
          | ((uint32_t)base[1] << 8)
          | ((uint32_t)base[2] << 16)
          | ((uint32_t)base[3] << 24);
    info.multiboot_total_size = total_size;
    if (total_size < 8U) {
        gBootInfo = info;
        return;
    }

    const uint8_t *tag_ptr = base + 8U;
    const uint8_t *end = base + total_size;

    while ((tag_ptr + sizeof(struct multiboot_tag)) <= end) {
        const struct multiboot_tag *tag = (const struct multiboot_tag *)tag_ptr;

        if (info.tag_trace.count < 32) {
            info.tag_trace.types[info.tag_trace.count++] = tag->type;
        }

        if (tag->type == MB_TAG_TYPE_END) {
            break;
        }

        if (tag->size < sizeof(struct multiboot_tag)) {
            break;
        }

        if (tag->type == MB_TAG_TYPE_FRAMEBUFFER &&
            tag->size >= sizeof(struct multiboot_tag_framebuffer)) {
            const struct multiboot_tag_framebuffer *fb =
                (const struct multiboot_tag_framebuffer *)tag;

            info.has_framebuffer = TRUE;
            info.framebuffer.address = fb->framebuffer_addr;
            info.framebuffer.pitch = fb->framebuffer_pitch;
            info.framebuffer.width = fb->framebuffer_width;
            info.framebuffer.height = fb->framebuffer_height;
            info.framebuffer.bpp = fb->framebuffer_bpp;
            info.framebuffer.type = fb->framebuffer_type;
            info.framebuffer.red_position = 0;
            info.framebuffer.red_mask_size = 0;
            info.framebuffer.green_position = 0;
            info.framebuffer.green_mask_size = 0;
            info.framebuffer.blue_position = 0;
            info.framebuffer.blue_mask_size = 0;

            if (fb->framebuffer_type == 1U) {
                info.framebuffer.red_position = fb->color_info.rgb.framebuffer_red_field_position;
                info.framebuffer.red_mask_size = fb->color_info.rgb.framebuffer_red_mask_size;
                info.framebuffer.green_position = fb->color_info.rgb.framebuffer_green_field_position;
                info.framebuffer.green_mask_size = fb->color_info.rgb.framebuffer_green_mask_size;
                info.framebuffer.blue_position = fb->color_info.rgb.framebuffer_blue_field_position;
                info.framebuffer.blue_mask_size = fb->color_info.rgb.framebuffer_blue_mask_size;
            }
        } else if (tag->type == MB_TAG_TYPE_MMAP && tag->size >= sizeof(struct multiboot_tag_mmap)) {
            const struct multiboot_tag_mmap *mm = (const struct multiboot_tag_mmap *)tag;
            info.memmap.entry_size = mm->entry_size;
            info.memmap.entry_version = mm->entry_version;
            info.memmap.count = 0;

            const uint8_t *ptr = tag_ptr + sizeof(struct multiboot_tag_mmap);
            const uint8_t *tend = tag_ptr + tag->size;
            while (ptr + mm->entry_size <= tend && info.memmap.count < 128) {
                const struct multiboot_mmap_entry *e = (const struct multiboot_mmap_entry *)ptr;
                BootMemRegion *dst = &info.memmap.regions[info.memmap.count++];
                dst->base = e->addr;
                dst->length = e->len;
                dst->type = e->type;
                dst->reserved = 0;
                ptr += mm->entry_size;
            }
            if (info.memmap.count > 0) info.has_memmap = TRUE;
        } else if ((tag->type == MB_TAG_TYPE_ACPI_OLD || tag->type == MB_TAG_TYPE_ACPI_NEW) &&
                   tag->size >= sizeof(struct multiboot_tag)) {
            const uint8_t *rsdp = tag_ptr + sizeof(struct multiboot_tag);
            uint32_t rsdp_len = tag->size - sizeof(struct multiboot_tag);
            if (rsdp_len >= 20U) {
                info.has_acpi = TRUE;
                info.acpi.rsdp = (const void *)rsdp;
                info.acpi.length = rsdp_len;
                info.acpi.revision = rsdp[15];
                info.acpi.is_xsdp = (tag->type == MB_TAG_TYPE_ACPI_NEW) ? 1U : 0U;
            }
        }

        size_t advance = align_up(tag->size, MB_ALIGN);
        if (advance == 0U || tag_ptr + advance > end) {
            break;
        }

        tag_ptr += advance;
    }

    gBootInfo = info;
}

ABI_C const BootInfo *BootInfoGet(void) {
    return &gBootInfo;
}

VOID* BootInfoGet_Wrapper(POINTER UnusedArg) {
    // Panggil fungsi asli
    return (VOID*)BootInfoGet();
}

ABI_C NORESULTFUNC MakeMultibootDriverInstance(){
    CPANSI_STRING DevName = "multiboot";

    DeviceManager::ObjectManager.RegisterDeviceInstance(
        DevName,
        DeviceManager::OT_OTHER,
        nullptr
    );

    DeviceManager::ObjectManager.RegisterFunctionToTable(
        DevName,
        REQ_MULTIBOOT_GET_INFO,
        (DeviceManager::HandleFunction)BootInfoGet_Wrapper
    );
}

// 1. Deklarasi label dari linker.ld (sekarang 64-bit pointers)
ABI_C void (*__init_array_start[])();
ABI_C void (*__init_array_end[])();

// 2. Deklarasi 'kernel_main' C++
ABI_C void KernelMain(VOID*);
ABI_C VOID IdleLoop(VOID*);

// 3. Fungsi 'lem' 64-bit
// Argumen 'mb_info_ptr' ini datang dari register 'rdi'
// (yang kita 'mov rdi, rbx' di asm)
ABI_C void KernelEntryPoint(uint64_t mb_info_ptr) {
    parse_multiboot2_info(mb_info_ptr);
    // Early paging + HHDM + kernel stack switch before running C++ global
    // constructors or entering KernelMain. These are C-linkage wrappers to
    // call into C++ Paging code from this C file.
    extern void Paging_Initialize_C(void);
    extern void Paging_RelocateGDT_C(void);
    extern void Paging_SwitchStack_C(void);
    Paging_Initialize_C();
    Paging_RelocateGDT_C();
    Paging_SwitchStack_C();

    Arch::ASM::Cli(); // Disable interrupts during init
    
    // (Di sini kamu bisa menyimpan mb_info_ptr ke variabel global
    //  untuk dibaca nanti oleh kernel_main, misal untuk memory map)

    // Panggil Global Constructor C++ (now executing on high kernel stack)
    size_t count = __init_array_end - __init_array_start;
    for (size_t i = 0; i < count; i++) {
        __init_array_start[i]();
    }

    if (DeviceManager::ObjectManager.FirstInitializeDevOBJManager()) {
        // Sukses init
    }
    MakeMultibootDriverInstance();

    // Deteksi Chipset 
    Firmware::Chipset::DetectMotherboardChipset();

    // INITIALIZE ACPI
    IDT::InitializeIDT();
    PIC::InitializePIC();
    PIC::Keyboard::InitializeKeyboardPIC();
    PIT::InitializePIT(100); // set PIT to 100 Hz (calibration/reference)
    // Enable IRQ-driven serial input (COM1 IRQ4)
    Serial::EnableIRQInput();

    // Initialize ACPI/MADT which will parse tables and initialize LAPIC/IOAPIC
    // (but do not start the LAPIC timer yet; we need interrupts enabled to
    // calibrate it against the PIT).
    ACPI::Initialize();

    // Enable interrupts so PIT IRQs will increment PIT::ticks for calibration
    Arch::Sti();

    // Calibrate and start LAPIC timer at 100 Hz (uses PIT ticks)
    // Use a distinct vector for LAPIC timer (not IRQ0/PIT vector 0x20)
    // to avoid conflicts with the PIT handler while calibration runs.
    ACPI::Timer::InitializeLapicTimer(CONFIG_TIMER_HEXA_GLOBAL, 100, TRUE);

    // Now mask and disable legacy PIC hardware while interrupts are briefly
    // disabled inside the call. After that, re-enable interrupts so LAPIC
    // delivered interrupts are accepted.
    PIC::DisableIRQWhileAndMaskOldPIC();
    Arch::Sti();

    // Panggil Kernel C++ 64-bit
    //KernelMain();
    // UPDATE: DO NOT CALL KERNELMAIN. kita akan buat kthread kernelmain.
    Tasking::CreateIdleTask(IdleLoop);

    Tasking::CreateKThread(KernelMain, 0, "KernelMainThread");

    // Timer sudah jalan disini, harusnya kita bisa start scheduler sekarang juga.
    Printk::Write(Printk::Level::LOG_INFO, "Starting Task Scheduler...\n");
    Tasking::SchedulerStart();

    // Hang
    for (;;) {
        asm("hlt");
    }
}