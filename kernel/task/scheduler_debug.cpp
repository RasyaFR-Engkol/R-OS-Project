extern "C" {
    #include <cpu_context.hpp>
}

#include <rossys.hpp>
#include <logging.hpp>

using namespace Printk;

extern "C" void DumpCpuContext(void *ctx) {
    if (!ctx) {
        Printk::Write(Level::LOG_ERR, "DumpCpuContext: null context\n");
        return;
    }

    CpuContext_T *c = (CpuContext_T*)ctx;
    Printk::Write(Level::LOG_INFO,
        "DumpCpuContext: RIP=0x%llx CS=0x%x RFLAGS=0x%llx RSP=0x%llx SS=0x%x\n",
        (unsigned long long)c->rip, (unsigned)c->cs,
        (unsigned long long)c->rflags, (unsigned long long)c->rsp,
        (unsigned)c->ss);

    Printk::Write(Level::LOG_INFO,
        "  regs: RAX=0x%llx RBX=0x%llx RCX=0x%llx RDX=0x%llx\n",
        (unsigned long long)c->rax, (unsigned long long)c->rbx,
        (unsigned long long)c->rcx, (unsigned long long)c->rdx);

    Printk::Write(Level::LOG_INFO,
        "  regs2: RSI=0x%llx RDI=0x%llx RBP=0x%llx R8=0x%llx\n",
        (unsigned long long)c->rsi, (unsigned long long)c->rdi,
        (unsigned long long)c->rbp, (unsigned long long)c->r8);

    // Decode GDT entries for CS and SS
    struct GDTR { unsigned short limit; unsigned long long base; } __attribute__((packed));
    GDTR gdtr;
    asm volatile("sgdt %0" : "=m"(gdtr));

    auto dump_selector = [&](unsigned sel, const char *name){
        unsigned index = sel >> 3;
        if (index * 8 + 7 > gdtr.limit) {
            Printk::Write(Level::LOG_INFO, "  %s: selector 0x%x index %u out of GDT limit\n", name, sel, index);
            return;
        }
        unsigned long long desc = ((unsigned long long*)gdtr.base)[index];
        unsigned access = (desc >> 40) & 0xff;
        unsigned flags = (desc >> 52) & 0xf;
        unsigned present = (access >> 7) & 1;
        unsigned dpl = (access >> 5) & 0x3;
        unsigned s = (access >> 4) & 1;
        unsigned type = access & 0xf;
        unsigned g = (flags >> 3) & 1;
        unsigned db = (flags >> 2) & 1;
        unsigned l = (flags >> 1) & 1;
        Printk::Write(Level::LOG_INFO,
            "  %s: raw=0x%016llx access=0x%x flags=0x%x P=%u DPL=%u S=%u TYPE=0x%x G=%u DB=%u L=%u\n",
            name, (unsigned long long)desc, access, flags, present, dpl, s, type, g, db, l);
    };

    dump_selector((unsigned)c->cs, "CS_desc");
    dump_selector((unsigned)c->ss, "SS_desc");
}
