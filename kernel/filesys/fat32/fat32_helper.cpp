#include "fat32.hpp"
#include <rosval.h>
#include <filesystem/filesystem.hpp>

#define PRINTK_MODULE_NAME "FatHelper"
#include <logging.hpp>
#include <string.hpp>
#include <mm.hpp>
#include <rostime.hpp>

namespace{
    U16 EncodeDateForFAT(U32 Year, U32 Month, U32 Day){
        U32 FatYear = (Year > 1980) ? (Year - 1980) : 0;

        if(FatYear > 127) FatYear = 127;

        return ((FatYear & 0x7F) << 9) | ((Month & 0x0F) << 5) | (Day & 0x1F);
    }

    U16 EncodeFATTime(U8 Hour, U8 Minute, U8 Second){
        return (((Hour & 0x1F) << 11) | ((Minute & 0x3F) << 5) | ((Second /2) & 0x1F));
    }
}

// Helper: convert UTF-16LE buffer (array of U16) to UTF-8 into out buffer.
// outSize is size of out buffer in bytes. Returns number of bytes written (excluding NUL) or -1 on truncation error.
static int UTF16LE_To_UTF8(const U16* src, int srcCount, char* out, int outSize){
    int outPos = 0;
    for(int i=0;i<srcCount;i++){
        U16 w = src[i];
        if(w == 0x0000) break;
        uint32_t codepoint;
        // handle surrogate pairs
        if(w >= 0xD800 && w <= 0xDBFF){
            if(i+1 < srcCount){
                U16 w2 = src[i+1];
                if(w2 >= 0xDC00 && w2 <= 0xDFFF){
                    codepoint = 0x10000 + (((w - 0xD800) << 10) | (w2 - 0xDC00));
                    i++; // consumed low surrogate
                } else {
                    codepoint = 0xFFFD; // replacement
                }
            } else {
                codepoint = 0xFFFD;
            }
        } else if(w >= 0xDC00 && w <= 0xDFFF){
            codepoint = 0xFFFD;
        } else {
            codepoint = w;
        }

        // encode to UTF-8
        if(codepoint <= 0x7F){
            if(outPos + 1 >= outSize) return -1;
            out[outPos++] = (char)codepoint;
        } else if(codepoint <= 0x7FF){
            if(outPos + 2 >= outSize) return -1;
            out[outPos++] = (char)(0xC0 | (codepoint >> 6));
            out[outPos++] = (char)(0x80 | (codepoint & 0x3F));
        } else if(codepoint <= 0xFFFF){
            if(outPos + 3 >= outSize) return -1;
            out[outPos++] = (char)(0xE0 | (codepoint >> 12));
            out[outPos++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
            out[outPos++] = (char)(0x80 | (codepoint & 0x3F));
        } else {
            if(outPos + 4 >= outSize) return -1;
            out[outPos++] = (char)(0xF0 | (codepoint >> 18));
            out[outPos++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
            out[outPos++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
            out[outPos++] = (char)(0x80 | (codepoint & 0x3F));
        }
    }
    if(outPos < outSize) out[outPos] = '\0';
    else out[outSize-1] = '\0';
    return outPos;
}

BOOL FAT32FileSystem::ParseBPB(U8 *BootSector){
    if(BootSector[510] != 0x55 || BootSector[511] != 0xAA){
        Printk::Write(Printk::Level::LOG_ERR, "FAT32: Invalid Boot Sector Signature.\n");
        return FALSE;
    }

    String::Memcpy(&m_BPB, BootSector, sizeof(FAT32_BPB));

    if(String::Memcmp(m_BPB.FSType, "FAT32   ", 8) != 0){
        Printk::Write(Printk::Level::LOG_ERR, "FAT32: Filesystem Type is not FAT32.\n");
        return FALSE;
    }

    if(m_BPB.BytesPerSector == 0 || m_BPB.SectorsPerCluster == 0){
        Printk::Write(Printk::Level::LOG_ERR, "FAT32: Invalid BytesPerSector or SectorsPerCluster in BPB.\n");
        return FALSE;
    }

    if(m_BPB.NumFATs == 0 || m_BPB.SectorsPerFAT32 == 0){
        Printk::Write(Printk::Level::LOG_ERR, "FAT32: Invalid NumFATs or SectorsPerFAT32 in BPB.\n");
        return FALSE;
    }

    return TRUE;
}

// --- Helper implementations ---

U64 FAT32FileSystem::ClusterToLBA(U32 cluster){
    if(cluster < 2) return 0;
    return m_FirstDataSectorLBA + ((U64)(cluster - 2) * (U64)m_BPB.SectorsPerCluster);
}

U32 FAT32FileSystem::GetNextCluster(U32 currentCluster){
    // FAT32 entries are 4 bytes each
    U64 fatOffset = (U64)currentCluster * 4ULL;
    U64 fatSector = m_FirstFATSectorLBA + (fatOffset / m_BPB.BytesPerSector);
    U32 offsetInSector = (U32)(fatOffset % m_BPB.BytesPerSector);

    PageAlloc::DMAAlloc::DMABuffer* buf = nullptr;
    if(!m_Partition->ReadSectors(fatSector, 1, &buf)){
        return 0x0FFFFFFF;
    }

    U8* data = (U8*)buf->VirtAddr;
    U32 entry = (U32)data[offsetInSector] | ((U32)data[offsetInSector+1] << 8) |
                ((U32)data[offsetInSector+2] << 16) | ((U32)data[offsetInSector+3] << 24);
    PageAlloc::DMAAlloc::FreeDMABuffer(buf);
    entry &= 0x0FFFFFFF; // mask to 28 bits
    return entry;
}

BOOL FAT32FileSystem::ReadCluster(U32 cluster, U8* buffer){
    U64 lba = ClusterToLBA(cluster);
    U32 sectors = m_BPB.SectorsPerCluster;
    PageAlloc::DMAAlloc::DMABuffer* buf = nullptr;
    if(!m_Partition->ReadSectors(lba, sectors, &buf)){
        return FALSE;
    }
    U32 bytes = (U32)m_BPB.BytesPerSector * sectors;
    String::Memcpy(buffer, (U8*)buf->VirtAddr, bytes);
    PageAlloc::DMAAlloc::FreeDMABuffer(buf);
    return TRUE;
}

BOOL FAT32FileSystem::WriteCluster(U32 cluster, U8* buffer){
    U64 lba = ClusterToLBA(cluster);
    U32 bytes = (U32)m_BPB.BytesPerSector * (U32)m_BPB.SectorsPerCluster;
    U32 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    PageAlloc::DMAAlloc::DMABuffer* buf = PageAlloc::DMAAlloc::AllocateDMAPages(pages);
    if(!buf) return FALSE;
    String::Memcpy((U8*)buf->VirtAddr, buffer, bytes);
    BOOL ok = m_Partition->WriteSectors(lba, m_BPB.SectorsPerCluster, buf);
    PageAlloc::DMAAlloc::FreeDMABuffer(buf);
    return ok;
}

BOOL FAT32FileSystem::FindFileInDir(const char* name, U32 dirStartCluster, FAT32_DirectoryEntry* entryOut,
                                    char* foundNameOut, unsigned long long foundNameOutSize,
                                    U64* entryLBAOut, U32* entryOffsetOut) // <-- TANDA TANGAN BARU
{
    if(!m_Partition || !name || !entryOut) return FALSE;

    // We'll iterate directory entries using ReadDirectory and compare names (case-insensitive)
    struct FindCtx {
        const char* want;
        BOOL found;
        FAT32_DirectoryEntry result;
        char foundName[520];
        U64 resultLBA;     // <-- TAMBAHAN
        U32 resultOffset;  // <-- TAMBAHAN
    } ctx;
    ctx.want = name;
    ctx.found = FALSE;
    ctx.foundName[0] = '\0';
    ctx.resultLBA = 0;
    ctx.resultOffset = 0;

    // !!! PERBARUI TANDA TANGAN LAMBDA INI !!!
    auto cb = [](const char* fname, FAT32_DirectoryEntry* de, void* vctx, U64 entryLBA, U32 entryOffset)->BOOL{
        FindCtx* fc = (FindCtx*)vctx;
        unsigned long long a_len = String::Strlen(fname);
        unsigned long long b_len = String::Strlen(fc->want);
        if(a_len != b_len) return TRUE; // continue
        
        // Perbandingan case-insensitive
        for(unsigned long long i=0;i<a_len;i++){
            char ca = fname[i]; if(ca >= 'a' && ca <= 'z') ca -= 32;
            char cbch = fc->want[i]; if(cbch >= 'a' && cbch <= 'z') cbch -= 32;
            if(ca != cbch) return TRUE; // continue
        }

        // matched
        String::Memcpy(&fc->result, de, sizeof(FAT32_DirectoryEntry));
        String::Strcpy(fc->foundName, fname);
        fc->resultLBA = entryLBA;     // <-- SIMPAN LOKASI
        fc->resultOffset = entryOffset; // <-- SIMPAN LOKASI
        fc->found = TRUE;
        return FALSE; // stop iteration
    };

    // Panggil ReadDirectory dengan callback baru
    BOOL ok = ReadDirectory(dirStartCluster, cb, &ctx);
    if(!ok) return FALSE;

    if(ctx.found){
        String::Memcpy(entryOut, &ctx.result, sizeof(FAT32_DirectoryEntry));
        if(foundNameOut && foundNameOutSize > 0){
            // copy up to foundNameOutSize-1
            unsigned long long tocpy = String::Strlen(ctx.foundName);
            if(tocpy >= foundNameOutSize) tocpy = foundNameOutSize - 1;
            String::Memcpy(foundNameOut, ctx.foundName, tocpy);
            ((char*)foundNameOut)[tocpy] = '\0';
        }
        
        // --- TAMBAHKAN INI ---
        // Kembalikan lokasi entri jika pointer-nya disediakan
        if(entryLBAOut) {
            *entryLBAOut = ctx.resultLBA;
        }
        if(entryOffsetOut) {
            *entryOffsetOut = ctx.resultOffset;
        }
        // --- AKHIR TAMBAHAN ---

        return TRUE;
    }
    return FALSE;
}

// ReadDirectory: iterate directory entries starting at dirStartCluster and call callback for each entry
// callback should return TRUE to continue, FALSE to stop iteration.
BOOL FAT32FileSystem::ReadDirectory(U32 dirStartCluster, 
                                  BOOL (*callback)(const char* name, FAT32_DirectoryEntry* de, void* ctx, U64 entryLBA, U32 entryOffset), 
                                  void* ctx)
{
    if(!m_Partition || !callback) return FALSE;

    U32 cluster = dirStartCluster;
    char lfn_buf[520]; // ample space for LFN
    lfn_buf[0] = '\0';
    BOOL have_lfn = FALSE;
    int lfn_chars_filled = 0;

    U32 bytesPerSector = (U32)m_BPB.BytesPerSector;

    while(cluster < 0x0FFFFFF8){
        U64 lba = ClusterToLBA(cluster); // LBA awal dari cluster ini
        if (lba == 0) {
            // Cluster 0 atau 1 (root dir di FAT16/12), atau invalid.
            // Di FAT32, root dir (klaster 2) harusnya ditangani ClusterToLBA.
            // Jika LBA 0, itu error.
            Printk::Write(Printk::Level::LOG_ERR, "FAT32: ReadDirectory dapat LBA 0 dari ClusterToLBA(%u)", cluster);
            return FALSE;
        }

        PageAlloc::DMAAlloc::DMABuffer* buf = nullptr;
        if(!m_Partition->ReadSectors(lba, m_BPB.SectorsPerCluster, &buf)) {
            Printk::Write(Printk::Level::LOG_ERR, "FAT32: ReadDirectory gagal baca LBA %llu", lba);
            return FALSE;
        }

        U8* data = (U8*)buf->VirtAddr;
        U32 clusterBytes = bytesPerSector * (U32)m_BPB.SectorsPerCluster;
        
        for(U32 off = 0; off + 32 <= clusterBytes; off += 32){
            U8 first = data[off];
            if(first == 0x00){
                PageAlloc::DMAAlloc::FreeDMABuffer(buf);
                return TRUE; // finished directory
            }
            U8 attr = data[off + 11];
            if(attr == 0x0F){
                U8 seqByte = data[off];
                int seqIndex = seqByte & 0x1F; // 1..20
                BOOL isLast = (seqByte & 0x40) != 0;

                if(seqIndex == 0 || seqIndex > 20){
                    // invalid sequence number; drop accumulated LFN state
                    lfn_buf[0] = '\0';
                    have_lfn = FALSE;
                    lfn_chars_filled = 0;
                    continue;
                }

                if(isLast){
                    String::Memset(lfn_buf, 0, sizeof(lfn_buf));
                    lfn_chars_filled = 0;
                }

                // LFN entry: extract its part (UTF-16LE) and map into final buffer based on sequence index
                U16 utf16_part[13]; int utf16_count = 0;
                for(int i=0;i<5;i++){
                    U16 ch = (U16)data[off + 1 + i*2] | ((U16)data[off + 1 + i*2 + 1] << 8);
                    if(ch == 0x0000 || ch == 0xFFFF) break;
                    utf16_part[utf16_count++] = ch;
                }
                for(int i=0;i<6;i++){
                    U16 ch = (U16)data[off + 14 + i*2] | ((U16)data[off + 14 + i*2 + 1] << 8);
                    if(ch == 0x0000 || ch == 0xFFFF) break;
                    utf16_part[utf16_count++] = ch;
                }
                for(int i=0;i<2;i++){
                    U16 ch = (U16)data[off + 28 + i*2] | ((U16)data[off + 28 + i*2 + 1] << 8);
                    if(ch == 0x0000 || ch == 0xFFFF) break;
                    utf16_part[utf16_count++] = ch;
                }

                char part_utf8[128];
                int res = UTF16LE_To_UTF8(utf16_part, utf16_count, part_utf8, (int)sizeof(part_utf8));
                if(res < 0) part_utf8[0] = '\0';

                int partlen = (int)String::Strlen(part_utf8);
                int startPos = (seqIndex - 1) * 13;
                if(startPos < (int)sizeof(lfn_buf) - 1){
                    int room = (int)sizeof(lfn_buf) - 1 - startPos;
                    int toCopy = partlen < room ? partlen : room;
                    for(int i = 0; i < toCopy; ++i){
                        lfn_buf[startPos + i] = part_utf8[i];
                    }
                    int endPos = startPos + toCopy;
                    if(endPos > lfn_chars_filled) lfn_chars_filled = endPos;
                    lfn_buf[lfn_chars_filled] = '\0';
                }
                have_lfn = TRUE;
                continue;
            }
            if(first == 0xE5) {
                // deleted entry; reset LFN buffer
                lfn_buf[0] = '\0'; have_lfn = FALSE; lfn_chars_filled = 0; continue;
            }

            // Regular SFN entry
            FAT32_DirectoryEntry* de = (FAT32_DirectoryEntry*)(data + off);
            char finalname[520]; finalname[0] = '\0';
            if(have_lfn){
                String::Strcpy(finalname, lfn_buf);
            } else {
                // build name from SFN 11 bytes
                int idx = 0;
                // Special case: '.' and '..' entries
                if(de->Name[0] == 0x2E){
                    // first byte '.' (0x2E). Determine if '.' or '..'
                    if(de->Name[1] == 0x2E) {
                        finalname[0] = '.'; finalname[1] = '.'; finalname[2] = '\0';
                    } else {
                        finalname[0] = '.'; finalname[1] = '\0';
                    }
                    idx = (int)String::Strlen(finalname);
                } else {
                    // base name (8 chars max)
                    for(int i=0;i<8;i++){
                        char c = de->Name[i];
                        if(c == ' ') break;
                        finalname[idx++] = c;
                    }
                    // extension (3 chars max)
                    int extStart = idx;
                    int extLen = 0;
                    for(int j=8;j<11;j++){
                        if(de->Name[j] == ' ') break;
                        if(extLen == 0 && idx < (int)sizeof(finalname)-1 && extStart > 0){
                            finalname[idx++] = '.';
                        }
                        if(idx < (int)sizeof(finalname)-1){
                            finalname[idx++] = de->Name[j];
                            extLen++;
                        }
                    }
                    finalname[idx] = '\0';
                }
                finalname[idx] = '\0';
            }

            // reset LFN buffer for next
            lfn_buf[0] = '\0'; have_lfn = FALSE; lfn_chars_filled = 0;

            // --- INI BAGIAN UTAMA YANG BERUBAH ---
            // Hitung LBA dan offset dari entri ini untuk callback
            // 'lba' adalah LBA awal cluster
            // 'off' adalah offset byte di dalam cluster
            U32 sectorIndexInCluster = off / bytesPerSector;
            U64 entryLBA = lba + sectorIndexInCluster;      // LBA Sektor absolut
            U32 entryOffset = off % bytesPerSector;         // Offset byte di dalam Sektor

            // Panggil callback BARU; jika itu mengembalikan FALSE, stop
            if(!callback(finalname, de, ctx, entryLBA, entryOffset)){
                PageAlloc::DMAAlloc::FreeDMABuffer(buf);
                return TRUE;
            }
            // --- AKHIR PERUBAHAN ---
        }

        PageAlloc::DMAAlloc::FreeDMABuffer(buf);
        cluster = GetNextCluster(cluster);
    }

    return TRUE;
}

BOOL FAT32FileSystem::ListDirectory(U32 dirStartCluster, DirListingEntry* outEntries, U32 maxEntries, U32* outCount){
    if(!outEntries || maxEntries == 0 || !outCount) return FALSE;
    struct ListCtx { DirListingEntry* out; U32 max; U32 idx; } ctx;
    ctx.out = outEntries; ctx.max = maxEntries; ctx.idx = 0;

    // !!! PERBARUI TANDA TANGAN LAMBDA INI !!!
    auto cb = [](const char* name, FAT32_DirectoryEntry* de, void* vctx, U64 entryLBA, U32 entryOffset)->BOOL{
        // Kita tidak butuh entryLBA/entryOffset di sini, tapi kita harus menerimanya
        (void)entryLBA;    // <-- Menandakan "tidak dipakai"
        (void)entryOffset; // <-- Menandakan "tidak dipakai"

        ListCtx* lc = (ListCtx*)vctx;
        if(lc->idx >= lc->max) return FALSE; // stop
        DirListingEntry* e = &lc->out[lc->idx++];
        unsigned long long nlen = String::Strlen(name);
        unsigned long long tocpy = nlen;
        if(tocpy >= sizeof(e->Name)) tocpy = sizeof(e->Name)-1;
        String::Memcpy(e->Name, name, tocpy);
        e->Name[tocpy] = '\0';
        String::Memcpy(&e->DirEntry, de, sizeof(FAT32_DirectoryEntry));
        return TRUE; // continue
    };

    BOOL ok = ReadDirectory(dirStartCluster, cb, &ctx);
    *outCount = ctx.idx;
    return ok;
}
// --- SFN / LFN creation helpers ---

U8 FAT32FileSystem::ComputeSFNChecksum(const U8 sfn[11]){
    U8 sum = 0;
    for(int i = 0; i < 11; ++i){
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + sfn[i];
    }
    return sum;
}

// Make a best-effort SFN (8.3) from a UTF-8 filename. Simple rules:
// - split at last '.' into base/ext; uppercase; replace invalid chars with '_'
// - if base > 8, produce first 6 chars + ~1
// - pad with spaces
BOOL FAT32FileSystem::MakeSFNFromName(const char* name, U8 out11[11]){
    if(!name || !out11) return FALSE;
    // find last dot
    const char* lastdot = nullptr;
    for(const char* p = name; *p; ++p) if(*p == '.') lastdot = p;

    const char* base = name;
    size_t baselen = lastdot ? (size_t)(lastdot - name) : String::Strlen(name);
    const char* ext = lastdot ? lastdot + 1 : nullptr;
    size_t extlen = ext ? String::Strlen(ext) : 0;

    // sanitize and uppercase
    char b[16]; size_t bi = 0;
    for(size_t i = 0; i < baselen && bi < sizeof(b)-1; ++i){
        unsigned char c = (unsigned char)base[i];
        if(c == ' ') continue;
        // illegal chars in SFN: +, , ; = [ ] and other control chars
        if(c < 0x20) continue;
        // map to underscore if problematic
        if(c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' || c == '\\' || c == '|') c = '_';
        if(c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        b[bi++] = (char)c;
    }
    b[bi] = '\0';

    char e[8]; size_t ei = 0;
    for(size_t i = 0; i < extlen && ei < 3; ++i){
        unsigned char c = (unsigned char)ext[i];
        if(c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        e[ei++] = (char)c;
    }
    e[ei] = '\0';

    // Build SFN
    // If base too long, use first 6 + ~1
    char sfn[12];
    for(int i=0;i<11;i++) sfn[i] = ' ';
    sfn[11] = '\0';

    if(bi <= 8){
        for(size_t i=0;i<bi;i++) sfn[i] = b[i];
    } else {
        // first 6 chars + ~1
        for(int i=0;i<6;i++) sfn[i] = b[i];
        sfn[6] = '~'; sfn[7] = '1';
    }
    // extension
    for(size_t i=0;i<ei && i<3;i++) sfn[8+i] = e[i];

    // copy to output as bytes
    for(int i=0;i<11;i++) out11[i] = (U8)sfn[i];
    return TRUE;
}

// Find a sequence of 'needed' contiguous free directory entries inside dirStartCluster.
// Returns TRUE and sets outEntryLBA and outEntryOffset (byte offset within sector) if found.
BOOL FAT32FileSystem::FindFreeDirSlots(U32 dirStartCluster, U32 needed, U64* outEntryLBA, U32* outEntryOffset){
    if(needed == 0) return FALSE;
    U32 cluster = dirStartCluster;
    U32 bytesPerSector = (U32)m_BPB.BytesPerSector;
    U32 sectorsPerCluster = m_BPB.SectorsPerCluster;
    // guard to avoid infinite loops on corrupted FAT chains
    U32 guard = 0;
    while(cluster < 0x0FFFFFF8){
        if(cluster < 2){
            Printk::Write(Printk::Level::LOG_ERR, "FAT32: FindFreeDirSlots encountered invalid cluster %u in chain (dirStart=%u)\n", cluster, dirStartCluster);
            return FALSE;
        }
        if(++guard > (m_TotalClusters + 2)){
            Printk::Write(Printk::Level::LOG_ERR, "FAT32: FindFreeDirSlots aborted after excessive iterations (dirStart=%u)\n", dirStartCluster);
            return FALSE;
        }
        U64 lbaCluster = ClusterToLBA(cluster);
        for(U32 s = 0; s < sectorsPerCluster; ++s){
            U64 sectorLBA = lbaCluster + s;
            PageAlloc::DMAAlloc::DMABuffer* buf = nullptr;
            if(!m_Partition->ReadSectors(sectorLBA, 1, &buf)) return FALSE;
            U8* data = (U8*)buf->VirtAddr;

            U32 consec = 0;
            U32 startOff = 0;
            for(U32 off = 0; off + 32 <= bytesPerSector; off += 32){
                U8 first = data[off];
                if(first == 0x00 || first == 0xE5){
                    if(consec == 0) startOff = off;
                    consec++;
                    if(consec >= needed){
                        // compute LBA/offset of first slot
                        *outEntryLBA = sectorLBA;
                        *outEntryOffset = startOff;
                        PageAlloc::DMAAlloc::FreeDMABuffer(buf);
                        return TRUE;
                    }
                } else {
                    consec = 0;
                }
            }
            PageAlloc::DMAAlloc::FreeDMABuffer(buf);
        }
            cluster = GetNextCluster(cluster);
    }
    return FALSE;
}

// Create LFN entries + SFN entry in directory. Returns TRUE on success.
BOOL FAT32FileSystem::CreateDirectoryEntry(U32 dirStartCluster, const char* name, U32 startCluster, U32 fileSize, U64* outEntryLBA, U32* outEntryOffset){
    if(!name || !m_Partition) return FALSE;

    // Convert name to basic UTF-16 (naive: only ASCII supported robustly)
    unsigned long long namelen = String::Strlen(name);
    U16 utf16[520]; unsigned int utf16_count = 0;
    for(unsigned long long i=0;i<namelen && utf16_count < (sizeof(utf16)/2 - 1); ++i){
        unsigned char c = (unsigned char)name[i];
        if(c < 0x80) utf16[utf16_count++] = (U16)c;
        else utf16[utf16_count++] = (U16)'?';
    }
    utf16[utf16_count] = 0;

    if (utf16_count > 255) {
        Printk::Write(Printk::Level::LOG_ERR, "FAT32: Filename too long: '%s' (%u chars > 255)\n", name, utf16_count);
        return FALSE; 
    }

    // number of LFN entries required: 13 UTF16 chars per LFN entry
    unsigned int lfn_entries = (utf16_count + 12) / 13;
    unsigned int total_needed = lfn_entries + 1; // +1 for SFN

    // Find free contiguous slots. If not found, try expanding the directory by
    // allocating+zeroing a new cluster and retrying up to a few times. This
    // handles fragmented directories and transient allocation/write failures.
    U64 foundLBA = 0; U32 foundOffset = 0;
    const int maxExpandAttempts = 4;
    int attempt = 0;
    while(true){
        if(FindFreeDirSlots(dirStartCluster, total_needed, &foundLBA, &foundOffset)){
            break; // found slots
        }

        if(++attempt > maxExpandAttempts){
            Printk::Write(Printk::Level::LOG_ERR, "FAT32: No contiguous free directory slots after %d expansion attempts for '%s'\n", attempt-1, name);
            return FALSE;
        }

        Printk::Write(Printk::Level::LOG_INFO, "FAT32: No contiguous slots found; expansion attempt %d for '%s'\n", attempt, name);

        // Find last cluster in chain
        U32 last = dirStartCluster;
        U32 guard = 0;
        while(true){
            U32 next = GetNextCluster(last);
            if(next >= 0x0FFFFFF8) break;
            if(next == 0) break; // corrupted chain
            last = next;
            if(++guard > m_TotalClusters + 2) break;
        }

        U32 newc = AllocateCluster(last == 0 ? 0 : last);
        if(newc == 0){
            Printk::Write(Printk::Level::LOG_ERR, "FAT32: Failed to allocate new cluster to expand directory for '%s' (attempt %d)\n", name, attempt);
            return FALSE;
        }
        Printk::Write(Printk::Level::LOG_INFO, "FAT32: Allocated new cluster %u to expand directory for '%s'\n", newc, name);

        // Zero the newly allocated cluster on disk so it appears as free entries
        U64 newLBA = ClusterToLBA(newc);
        U32 sectors = m_BPB.SectorsPerCluster;
        U32 pages = (sectors * m_BPB.BytesPerSector + PAGE_SIZE - 1) / PAGE_SIZE;
        PageAlloc::DMAAlloc::DMABuffer* zbuf = PageAlloc::DMAAlloc::AllocateDMAPages(pages);
        if(!zbuf){
            Printk::Write(Printk::Level::LOG_ERR, "FAT32: Failed to allocate DMA buffer to zero new directory cluster for '%s'\n", name);
            return FALSE;
        }
        // zero and write exactly sectors worth of bytes
        String::Memset((void*)zbuf->VirtAddr, 0, (U32)sectors * m_BPB.BytesPerSector);
        if(!m_Partition->WriteSectors(newLBA, sectors, zbuf)){
            Printk::Write(Printk::Level::LOG_ERR, "FAT32: Failed to write zeroed new directory cluster LBA %llu for '%s'\n", newLBA, name);
            PageAlloc::DMAAlloc::FreeDMABuffer(zbuf);
            return FALSE;
        }
        PageAlloc::DMAAlloc::FreeDMABuffer(zbuf);

        // loop and try FindFreeDirSlots again
    }

    // Generate SFN
    U8 sfn[11];
    if(!MakeSFNFromName(name, sfn)) return FALSE;
    U8 chk = ComputeSFNChecksum(sfn);

    // We'll write entries starting at foundLBA/foundOffset; LFN entries are written in reverse order
    // Keep a small sector cache to avoid rereading the same sector repeatedly
    U64 curSectorLBA = (U64)-1; PageAlloc::DMAAlloc::DMABuffer* curBuf = nullptr;

    auto loadSector = [&](U64 lba)->BOOL{
        if(curBuf && curSectorLBA == lba) return TRUE;
        if(curBuf) { PageAlloc::DMAAlloc::FreeDMABuffer(curBuf); curBuf = nullptr; }
        if(!m_Partition->ReadSectors(lba, 1, &curBuf)) return FALSE;
        curSectorLBA = lba; return TRUE;
    };

    auto flushSector = [&](){ if(curBuf){ BOOL ok = m_Partition->WriteSectors(curSectorLBA, 1, curBuf); PageAlloc::DMAAlloc::FreeDMABuffer(curBuf); curBuf = nullptr; curSectorLBA = (U64)-1; return ok; } return TRUE; };

    bool insertedTerminator = false; // ensure exactly one UTF-16 terminator is written after the name
    // fill LFN entries
    for(unsigned int idx = 0; idx < lfn_entries; ++idx){
        unsigned int part = lfn_entries - idx; // sequence number: lfn_entries .. 1
        // prepare 32-byte entry buffer
        U8 entry32[32]; for(int i=0;i<32;i++) entry32[i] = 0xFF;
        // sequence number
        U8 seq = (U8)part;
        if(part == lfn_entries) seq |= 0x40; // last LFN flag
        entry32[0] = seq;
        // attribute
        entry32[11] = 0x0F;
        // type and checksum
        entry32[12] = 0x00;
        entry32[13] = chk;
        // first cluster always 0
        entry32[26] = 0x00; entry32[27] = 0x00;

        // Fill name parts (13 UTF16 chars) from utf16 buffer.
        // Sequence index 1 (closest to SFN) stores the first 13 characters, so
        // the start offset is (part-1)*13 when counting backwards.
        int startChar = (int)((part - 1) * 13);
        // Name1: 5 chars -> offsets 1..10 (little endian)
        for(int i=0;i<5;i++){
            int ci = startChar + i;
            U16 wc;
            if(ci < (int)utf16_count){ wc = utf16[ci]; }
            else if(!insertedTerminator){ wc = 0x0000; insertedTerminator = true; }
            else { wc = 0xFFFF; }
            entry32[1 + i*2] = (U8)(wc & 0xFF);
            entry32[1 + i*2 + 1] = (U8)((wc >> 8) & 0xFF);
        }
        // Name2: 6 chars -> offsets 14..25
        for(int i=0;i<6;i++){
            int ci = startChar + 5 + i;
            U16 wc;
            if(ci < (int)utf16_count){ wc = utf16[ci]; }
            else if(!insertedTerminator){ wc = 0x0000; insertedTerminator = true; }
            else { wc = 0xFFFF; }
            entry32[14 + i*2] = (U8)(wc & 0xFF);
            entry32[14 + i*2 + 1] = (U8)((wc >> 8) & 0xFF);
        }
        // Name3: 2 chars -> offsets 28..31
        for(int i=0;i<2;i++){
            int ci = startChar + 11 + i;
            U16 wc;
            if(ci < (int)utf16_count){ wc = utf16[ci]; }
            else if(!insertedTerminator){ wc = 0x0000; insertedTerminator = true; }
            else { wc = 0xFFFF; }
            entry32[28 + i*2] = (U8)(wc & 0xFF);
            entry32[28 + i*2 + 1] = (U8)((wc >> 8) & 0xFF);
        }

        // compute destination position for this LFN entry: it's before the SFN slot
        // overall slot index j from 0..total_needed-1 where last entry (j = total_needed-1) is SFN
        unsigned int lfn_j = idx; // 0..lfn_entries-1, corresponds to earlier slots
        U32 bytePos = foundOffset + lfn_j * 32;
        U64 targetSector = foundLBA + (bytePos / (U32)m_BPB.BytesPerSector);
        U32 offsetInSector = bytePos % (U32)m_BPB.BytesPerSector;

        if(!loadSector(targetSector)) return FALSE;
        // copy entry32 into curBuf at offsetInSector
        U8* data = (U8*)curBuf->VirtAddr;
        for(int z=0; z<32; ++z) data[offsetInSector + z] = entry32[z];
        // write back this sector now
        if(!flushSector()) return FALSE;
    }

    // Now write SFN entry into last slot (index = lfn_entries)
    U32 sfnBytePos = foundOffset + lfn_entries * 32;
    U64 sfnSector = foundLBA + (sfnBytePos / (U32)m_BPB.BytesPerSector);
    U32 sfnOffset = sfnBytePos % (U32)m_BPB.BytesPerSector;
    if(!loadSector(sfnSector)) return FALSE;
    U8* sdata = (U8*)curBuf->VirtAddr;
    // build SFN 32-byte entry
    U8 sfnbuf[32]; for(int i=0;i<32;i++) sfnbuf[i] = 0x00;
    // name 11
    for(int i=0;i<11;i++) sfnbuf[i] = sfn[i];
    sfnbuf[11] = 0x20; // attribute: archive (regular file)
    sfnbuf[12] = 0x00; // NT reserved
    // times left zero
    // cluster high/low
    sfnbuf[20] = (U8)((startCluster >> 16) & 0xFF);
    sfnbuf[21] = (U8)((startCluster >> 24) & 0xFF); // cluster high stored at 0x14/0x15 in FAT32_DirectoryEntry but in SFN layout bytes 20/21
    sfnbuf[26] = (U8)(startCluster & 0xFF);
    sfnbuf[27] = (U8)((startCluster >> 8) & 0xFF);
    // file size at offset 28..31
    sfnbuf[28] = (U8)(fileSize & 0xFF);
    sfnbuf[29] = (U8)((fileSize >> 8) & 0xFF);
    sfnbuf[30] = (U8)((fileSize >> 16) & 0xFF);
    sfnbuf[31] = (U8)((fileSize >> 24) & 0xFF);

    for(int z=0; z<32; ++z) sdata[sfnOffset + z] = sfnbuf[z];
    if(!flushSector()) return FALSE;

    if(outEntryLBA) *outEntryLBA = sfnSector;
    if(outEntryOffset) *outEntryOffset = sfnOffset;
    return TRUE;
}

BOOL FAT32FileSystem::SetNextCluster(U32 Cluster, U32 NextClusterValue) {
    U32 BytesPerSector = m_BPB.BytesPerSector;
    U32 EntriesPerSector = BytesPerSector / 4;

    U64 FatSector = m_FirstFATSectorLBA + (Cluster / EntriesPerSector);
    U32 OffsetInSector = Cluster % EntriesPerSector;
    UNUSED__ U32 OffsetInBytes = OffsetInSector * 4;

    PageAlloc::DMAAlloc::DMABuffer* buf = nullptr;
    if(!m_Partition->ReadSectors(FatSector, 1, &buf)){
        Printk::Write(Printk::Level::LOG_ERR, "FAT32: Failed to read FAT sector for SetNextCluster\n");
        return FALSE;
    }

    U32 *FatData = (U32*)buf->VirtAddr;
    FatData[OffsetInSector] = (NextClusterValue & 0x0FFFFFFF); // Mask to 28 bits

    BOOL Success = TRUE;
    // Write the modified FAT sector to every FAT copy.
    // BUGFIX: Previously this used m_FirstDataSectorLBA which corrupts data region.
    // Correct target LBA for FAT copy i is: m_FirstFATSectorLBA + i*SectorsPerFAT32 + (FatSector - m_FirstFATSectorLBA)
    U64 LBAOffset = FatSector - m_FirstFATSectorLBA; // sector index inside a FAT
    for(U8 i = 0; i < m_BPB.NumFATs; i++){
        U64 FatBase = m_FirstFATSectorLBA + ((U64)i * (U64)m_BPB.SectorsPerFAT32);
        U64 TargetLBA = FatBase + LBAOffset;
        if(!m_Partition->WriteSectors(TargetLBA, 1, buf)) {
            Printk::Write(Printk::Level::LOG_ERR, "FAT32: Failed to write FAT sector copy %u (LBA %llu) for SetNextCluster\n", i, TargetLBA);
            Success = FALSE;
        }
    }

    PageAlloc::DMAAlloc::FreeDMABuffer(buf);
    return Success;
}

VOID FAT32FileSystem::ReadFSInfo(){
    if(m_BPB._fsInfoCluster == 0 || m_BPB._fsInfoCluster == 0xFFFF){
        Printk::Write(Printk::Level::LOG_INFO, "FAT32: No FSInfo sector defined.\n");
        return;
    }

    PageAlloc::DMAAlloc::DMABuffer *Buf = nullptr;
    if(!m_Partition->ReadSectors(m_BPB._fsInfoCluster, 1, &Buf)){
        Printk::Write(Printk::Level::LOG_ERR, "FAT32: Failed to read FSInfo sector.\n");
        return;
    }

    FAT32_FSInfo *FSInfo = (FAT32_FSInfo*)Buf->VirtAddr;

    // Read 16-bit little-endian word at offset 0x1FE safely (avoid misaligned cast)
    U8 b0 = *((U8*)Buf->VirtAddr + 0x1FE);
    U8 b1 = *((U8*)Buf->VirtAddr + 0x1FF);
    U16 SectorEndVal = (U16)b0 | ((U16)b1 << 8);

    if(FSInfo->LeadSignature != 0x41615252 || FSInfo->StrucSignature != 0x61417272 || SectorEndVal != 0xAA55){
        Printk::Write(Printk::Level::LOG_ERR, "FAT32: Invalid FSInfo sector signatures.\n");
        PageAlloc::DMAAlloc::FreeDMABuffer(Buf);
        return;
    } else {
        if(FSInfo->NextFreeCluster >= 2 && FSInfo->NextFreeCluster < m_TotalClusters) {
            m_NextFreeClusterHint = FSInfo->NextFreeCluster;
            Printk::Write(Printk::Level::LOG_INFO, "FAT32: FSInfo NextFreeCluster: %u\n", m_NextFreeClusterHint);
        } else {
            m_NextFreeClusterHint = 2;
        }
    }

    PageAlloc::DMAAlloc::FreeDMABuffer(Buf);
}

VOID FAT32FileSystem::UpdateFSInfoHint(U32 NewHint){
    m_NextFreeClusterHint = NewHint;

    if(m_BPB._fsInfoCluster == 0 || m_BPB._fsInfoCluster == 0xFFFF){
        Printk::Write(Printk::Level::LOG_INFO, "FAT32: No FSInfo sector defined; cannot update hint.\n");
        return;
    }

    PageAlloc::DMAAlloc::DMABuffer *Buf = nullptr;
    if(!m_Partition->ReadSectors(m_BPB._fsInfoCluster, 1, &Buf)){
        Printk::Write(Printk::Level::LOG_ERR, "FAT32: Failed to read FSInfo sector for update.\n");
        return;
    }

    FAT32_FSInfo *FSInfo = (FAT32_FSInfo*)Buf->VirtAddr;
    U8 b0 = *((U8*)Buf->VirtAddr + 0x1FE);
    U8 b1 = *((U8*)Buf->VirtAddr + 0x1FF);
    U16 SectorEndVal = (U16)b0 | ((U16)b1 << 8);

    if(FSInfo->LeadSignature == 0x41615252 && FSInfo->StrucSignature == 0x61417272 && SectorEndVal == 0xAA55){
        FSInfo->NextFreeCluster = NewHint;

        if(!m_Partition->WriteSectors(m_BPB._fsInfoCluster, 1, Buf)){
            Printk::Write(Printk::Level::LOG_ERR, "FAT32: Failed to write FSInfo sector for update.\n");
        }
    }

    PageAlloc::DMAAlloc::FreeDMABuffer(Buf);
}

U32 FAT32FileSystem::FindFreeCluster(){
    U32 StartCluster = m_NextFreeClusterHint;
    U32 FoundFreeCluster = 0;

    if(StartCluster < 2 || StartCluster >= m_TotalClusters){
        StartCluster = 2;
    }

    for(U32 Cluster = StartCluster; Cluster < m_TotalClusters; Cluster++){
        U32 Value = GetNextCluster(Cluster);
        if(Value == 0x00000000){
            FoundFreeCluster = Cluster;
            goto FoundCluster;
        }
    }

    for (U32 cluster = 2; cluster < StartCluster; cluster++) {
        U32 value = GetNextCluster(cluster);
        if (value == 0x00000000) {
            FoundFreeCluster = cluster;
            goto FoundCluster; // Ditemukan!
        }
    }

    Printk::Write(Printk::Level::LOG_ERR, "FAT32: No free clusters available.\n");
    return 0; // Tidak ada cluster bebas

FoundCluster:
    if(SetNextCluster(FoundFreeCluster, 0x0FFFFFF8)){
        UpdateFSInfoHint(FoundFreeCluster + 1);
        return FoundFreeCluster;
    } else {
        Printk::Write(Printk::Level::LOG_ERR, "FAT32: Failed to allocate cluster %u.\n", FoundFreeCluster);
        return 0;
    }
}

U32 FAT32FileSystem::AllocateCluster(U32 LastCluster){
    U32 NewCluster = FindFreeCluster();
    if(NewCluster == 0) {
        Printk::Write(Printk::Level::LOG_ERR, "FAT32: AllocateCluster failed to find free cluster.\n");
        return 0;
    }

    if(LastCluster != 0){
        if(!SetNextCluster(LastCluster, NewCluster)){
            Printk::Write(Printk::Level::LOG_ERR, "FAT32: AllocateCluster failed to link new cluster.\n");
            // TODO: Buat biar cluster ga jadi yatim
            return 0;
        }
    }

    return NewCluster;
}

BOOL FAT32FileSystem::UpdateDirectoryEntry(File* file){
    if(!file) return FALSE;

    if(file->Internal_DirEntryLBA == 0){
        return TRUE;
    }

    PageAlloc::DMAAlloc::DMABuffer *Buf = nullptr;
    if(!m_Partition->ReadSectors(file->Internal_DirEntryLBA, 1, &Buf)){
        Printk::Write(Printk::Level::LOG_ERR, "FAT32: Failed to read sector for updating directory entry.\n");
        return FALSE;
    }

    U8 *SectorData = (U8*)Buf->VirtAddr;

    FAT32_DirectoryEntry *DirEntry = (FAT32_DirectoryEntry*)(SectorData + file->Internal_DirEntryOffset);

    DirEntry->FileSize = file->FileSize;
    DirEntry->ClusterLow = (U16)(file->Internal_StartCluster & 0xFFFF);
    DirEntry->ClusterHigh = (U16)((file->Internal_StartCluster >> 16) & 0xFFFF);

    auto RTCTime = Arch::CMOS::ReadRTC();

    DirEntry->LastAccessDate = EncodeDateForFAT(RTCTime.year, RTCTime.month, RTCTime.day);
    DirEntry->LastWriteDate = EncodeDateForFAT(RTCTime.year, RTCTime.month, RTCTime.day);
    DirEntry->LastWriteTime = EncodeFATTime(RTCTime.hour, RTCTime.minute, RTCTime.second);
    DirEntry->CreationDate = EncodeFATTime(RTCTime.year, RTCTime.month, RTCTime.day);
    DirEntry->CreationTime = EncodeFATTime(RTCTime.hour, RTCTime.minute, RTCTime.second);

    BOOL Success = m_Partition->WriteSectors(file->Internal_DirEntryLBA, 1, Buf);
    if(!Success){
        Printk::Write(Printk::Level::LOG_ERR, "FAT32: Failed to write updated directory entry back to disk.\n");
    }

    PageAlloc::DMAAlloc::FreeDMABuffer(Buf);
    return Success;
}

// Release all FAT32 clusters in a chain starting at StartCluster by clearing
// their FAT entries to 0 (free). Updates FSInfo hint for faster future allocs.
BOOL FAT32FileSystem::FreeClusterChain(U32 StartCluster){
    if(StartCluster < 2) return TRUE; // nothing to do

    U32 cur = StartCluster;
    BOOL allOk = TRUE;
    U32 minFreed = cur;
    U32 guard = 0; // safety to avoid infinite loop on corrupted chains

    while(cur >= 2 && cur < 0x0FFFFFF8){
        if(cur < minFreed) minFreed = cur;
        U32 next = GetNextCluster(cur);
        if(!SetNextCluster(cur, 0)){
            allOk = FALSE;
        }
        cur = next;
        if(++guard > (m_TotalClusters + 2)) { allOk = FALSE; break; }
    }

    UpdateFSInfoHint(minFreed);
    return allOk;
}