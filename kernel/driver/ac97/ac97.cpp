#include "ac97.hpp"
#include "../pci/pci.hpp"
#include "port.hpp"
#include <logging.hpp>
#include <mm.hpp>

namespace AC97{
    static U32 g_Nambar = 0;
    static U32 g_Nabmbar = 0;

    NORESULTFUNC Initialize(U8 Bus, U8 Dev, U8 Func) {
        g_Nambar = PCI::ReadDword(Bus, Dev, Func, 0x10) & ~1;
        g_Nabmbar = PCI::ReadDword(Bus, Dev, Func, 0x14) & ~1;

        // 1. Enable Bus Master & IO (Wajib di awal)
        U16 Command = PCI::ReadWord(Bus, Dev, Func, 0x04);
        PCI::WriteWord(Bus, Dev, Func, 0x04, Command | (1 << 2) | (1 << 0));

        // --- LANGKAH BARU: HARD RESET ---
        
        // A. Reset Mixer (NAMBAR + 0x00)
        // Menulis value apapun ke sini akan mereset register mixer ke default.
        Port::Outw(g_Nambar + 0x00, 0xFFFF);
        
        // Tunggu sebentar (hardware butuh waktu buat bangun tidur)
        // Kalau lo punya fungsi delay, pake delay(10). Kalau gak, loop kosong aja.
        for(int i=0; i<10000; i++);

        // B. Global Reset Bus Master (NABMBAR + 0x2B - Global Control)
        // Bit 1 = Cold Reset. Tulis 2, tunggu, lalu tulis 0.
        Port::Outl(g_Nabmbar + 0x2B, 0x0002);
        for(int i=0; i<10000; i++);
        Port::Outl(g_Nabmbar + 0x2B, 0x0000);
        
        // --- AKHIR RESET ---

        // 2. Setup Volume (Ulangi lagi karena abis di-reset dia jadi Mute)
        Port::Outw(g_Nambar + 0x02, 0x0000); // Master Unmute
        Port::Outw(g_Nambar + 0x18, 0x0808); // PCM Out Unmute (0dB)
        
        // 3. Set Sample Rate (Penting! Reset bikin ini balik ke 48k atau random)
        Port::Outw(g_Nambar + 0x2C, 48000);

        Printk::Write(Printk::Level::LOG_INFO, "AC97: Hard Reset Complete & Volume Set.\n");
    }

    void PlayTestSound(U32 hz) {
        using namespace PageAlloc::DMAAlloc;

        // 1. Alokasikan BDL (Buffer Descriptor List) - Harus Physical Contiguous
        DMABuffer* bdl_buf = AllocateDMABytes(sizeof(AC97_BufferDescriptor) * 32);
        
        // 2. Alokasikan Buffer Suara (misal 64KB)
        DMABuffer* audio_buf = AllocateDMABytes(65536);

        // 3. Isi buffer dengan gelombang kotak (Square Wave) dengan frekuensi `hz`
        INT16* samples = (INT16*)audio_buf->VirtAddr;
    
        // Hardcode 48000 aja buat math, karena register 0x2C kadang bohong kalau VRA mati
        U32 sample_rate = 48000; 
        
        // Hitung Period
        // sample_rate / hz = jumlah frame dalam satu siklus gelombang
        int period = (int)(sample_rate / (hz ? hz : 440));
        if (period < 2) period = 2;

        // KITA ISI PER FRAME (Sepasang L & R)
        // Jadi loop-nya setengah dari total buffer karena 1 kali loop ngisi 2 tempat
        int num_frames = 32768 / 2; 

        for (int i = 0; i < num_frames; i++) {
            // Generate gelombang
            INT16 val = (i % period < (period / 2)) ? 5000 : -5000;
            
            // Masukkan ke Stereo (Interleaved)
            samples[i * 2]     = val; // Left Channel
            samples[i * 2 + 1] = val; // Right Channel
        }

        // 4. Setup Descriptor pertama
        AC97_BufferDescriptor* bdl = (AC97_BufferDescriptor*)bdl_buf->VirtAddr;
        bdl[0].Pointer = (U32)audio_buf->PhysAddr; // Alamat fisik
        bdl[0].Length  = 32768;                   // Jumlah sample
        bdl[0].Flags   = (1 << 15);               // Interrupt on Completion (IOC)

        // 5. Kirim ke Hardware (NABMBAR / BAR1)
        Port::Outl(g_Nabmbar + PCM_OUT_BDBAR, (U32)bdl_buf->PhysAddr);
        Port::Outb(g_Nabmbar + PCM_OUT_LVI, 0); // Last Valid Index = 0 (cuma 1 buffer)
        
        // 6. JALANKAN! Set bit 0 (Run) pada Control Register
        U8 cr = Port::Inb(g_Nabmbar + PCM_OUT_CR);
        Port::Outb(g_Nabmbar + PCM_OUT_CR, cr | 0x01);

        Printk::Write(Printk::Level::LOG_INFO, "AC97: Playback Started!\n");
    }
}