#include <rosval.h>
#include <rossys.hpp>
#include <mm.hpp>
#include <cpu_context.hpp>
#include <filesystem/filesystem.hpp>
#include <task.hpp>
#include <filesystem/linux_dirent.hpp>

// forward declaration for CanonicalizePath defined later in this file
VOID CanonicalizePath(const char* cwd, const char* input, char* output);

//
// FS.cpp : Mengikuti aturan konvensi linux
// 
// Mengikuti konvensi syscall Linux untuk operasi filesystem
// Referensi: https://syscalls.w3challs.com/?arch=x86_64
//
// fs.cpp: implementasi syscall filesystem
// 0: sys_read
// 1: sys_write
// 2: sys_open
// 3: sys_close
// 4: sys_stat
// 5: sys_fstat
// 6: sys_lstat
// 7: sys_poll
// 8: sys_lseek

VOID Sys_Read(CpuContext_T *CPUContext){
    U64 fd = CPUContext->rdi;
    U64 buf = CPUContext->rsi;
    U64 count = CPUContext->rdx;

    VOID *Buffer = Kmalloc::Alloc(count);
    if(!Buffer){
        CPUContext->rax = (U64)(-1); // Error
        return;
    }

    Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();
    if (!Curtask) {
        Kmalloc::Free(Buffer);
        CPUContext->rax = (U64)(-1);
        return;
    }

    // Asumsi udah kebuka oleh sys_open
    // File *FileDescriptor = (File*)fd;
    
    // FIX: Ambil dari FDTable milik task
    if (fd >= MAX_FILE_IN_PROCESS || !Curtask->FDTable[fd]) {
        Kmalloc::Free(Buffer);
        CPUContext->rax = (U64)(-1); // Bad file descriptor
        return;
    }
    File *FileDescriptor = Curtask->FDTable[fd];

    SIZE_T ReadBytes = VFSManager::Read(FileDescriptor, (U8*)Buffer, count);
    if(ReadBytes == (SIZE_T)(-1)){
        Kmalloc::Free(Buffer);
        CPUContext->rax = (U64)(-1); // Error
        return;
    }

    // Copy ke user buffer
    // Build user PML4 pointer from saved CR3 in the CPU context. CR3 is
    // expected to contain the physical CR3 for the user address space.
    U64 *user_pml4 = HHDM_PhysToVirt((UPTR)Curtask->CR3);

    if (!PageAlloc::CopyToUser(user_pml4, (void*)buf, Buffer, ReadBytes)) {
        Kmalloc::Free(Buffer);
        CPUContext->rax = (U64)(-1);
        return;
     }

    // Success: return number of bytes read
    Kmalloc::Free(Buffer);
    CPUContext->rax = (U64)ReadBytes;
}

VOID Sys_Write(CpuContext_T *CPUContext){
    U64 fd = CPUContext->rdi;
    UNUSED__ U64 buf = CPUContext->rsi;
    U64 count = CPUContext->rdx;

    VOID *Buffer = Kmalloc::Alloc(count);
    if(!Buffer){
        CPUContext->rax = (U64)(-1); // Error
        return;
    }

    Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();
    if (!Curtask) {
        Kmalloc::Free(Buffer);
        CPUContext->rax = (U64)(-1);
        return;
    }

    // Build user PML4 pointer from saved CR3 in the Task. CR3 must be
    // a valid physical frame (non-zero).
    if (Curtask->CR3 == 0) {
        Kmalloc::Free(Buffer);
        CPUContext->rax = (U64)(-1);
        Printk::Write(Printk::Level::LOG_ERR, "Sys_Write: invalid CR3 (0) for current task\n");
        return;
    }

    U64 *user_pml4 = HHDM_PhysToVirt(Curtask->CR3);

    if (!PageAlloc::CopyFromUser(user_pml4, Buffer, (void*)buf, count)) {
        Kmalloc::Free(Buffer);
        CPUContext->rax = (U64)(-1);
        Printk::Write(Printk::Level::LOG_ERR, "Sys_Write: CopyFromUser failed (buf=%p count=%llu CR3=0x%llx)\n",
                      (void*)buf, (unsigned long long)count, (unsigned long long)Curtask->CR3);
        return;
    }

    // Asumsi udah kebuka oleh sys_open
    File *FileDecsriptor = Curtask->FDTable[fd];

    SIZE_T WrittenBytes = VFSManager::Write(FileDecsriptor, (U8*)Buffer, count);
    if(WrittenBytes == (SIZE_T)(-1)){
        Kmalloc::Free(Buffer);
        CPUContext->rax = (U64)(-1); // Error
        return;
    }

    // Success: return number of bytes written
    Kmalloc::Free(Buffer);
    CPUContext->rax = (U64)WrittenBytes;
}

VOID Sys_Open(CpuContext_T *CPUContext){
    U64 pathname_ptr =CPUContext->rdi;
    U64 flags = CPUContext->rsi;
    UNUSED__ U64 mode = CPUContext->rdx;

    // Copy pathname dari user
    CHAR8 PathName[256];
    String::Memset(PathName, 0, sizeof(PathName));
    
    Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();
    if (!Curtask) { CPUContext->rax = (U64)(-1); return; }

    // Build user PML4 pointer from saved CR3 in the Task. CR3 must be
    // a valid physical frame (non-zero).
    if (Curtask->CR3 == 0) {
        CPUContext->rax = (U64)(-1);
        Printk::Write(Printk::Level::LOG_ERR, "Sys_Open: invalid CR3 (0) for current task\n");
        return;
    }

    U64 *user_pml4 = HHDM_PhysToVirt(Curtask->CR3);
    UNUSED__ BOOL copy_success = false;
    for (int i = 0; i < (int)sizeof(PathName) - 1; i++) {
        char c;
        // Copy 1 byte saja dari user
        if (!PageAlloc::CopyFromUser(user_pml4, &c, (void*)(pathname_ptr + i), 1)) {
            // Kalau gagal baca di tengah jalan, berarti segfault
            CPUContext->rax = (U64)(-1); 
            Printk::Write(Printk::Level::LOG_ERR, "Sys_Open: Segfault reading path at offset %d\n", i);
            return;
        }

        PathName[i] = c;
        
        // Kalau ketemu null terminator, stop! Kita sudah dapat stringnya.
        if (c == '\0') {
            copy_success = true;
            break;
        }
    }
    
    // Safety: Pastikan null terminated kalau string kepanjangan
    PathName[sizeof(PathName) - 1] = '\0';
    // Canonicalize relative paths against task CWD so VFS (kernel) receives absolute path
    CHAR8 FinalPath[256];
    CanonicalizePath(Curtask->CWD, (const char*)PathName, (char*)FinalPath);

    File *OpenedFile = VFSManager::Open((const char*)FinalPath);
    if(!OpenedFile){
        if(flags & 64){
            OpenedFile = VFSManager::CreateWithParents((const char*)FinalPath);
        }
    }

    if(!OpenedFile){
        CPUContext->rax = (U64)(-1); 
        return;
    }

    INTN FdIDX = -1;
    for(INTN i = 0; i < MAX_FILE_IN_PROCESS; i++){
        if(Curtask->FDTable[i] == nullptr){
            FdIDX = i;
            break;
        }
    }

    if(FdIDX == -1){
        VFSManager::Close(OpenedFile);
        CPUContext->rax = (U64)(-1); // No available FD
        return;
    }

    Curtask->FDTable[FdIDX] = OpenedFile;
    // Increment RefCount
    OpenedFile->RefCount++;
    CPUContext->rax = (U64)FdIDX; // Success
}

VOID Sys_Close(CpuContext_T *CPUContext){
    U64 fd = CPUContext->rdi;

    Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();
    if (!Curtask || fd >= MAX_FILE_IN_PROCESS || !Curtask->FDTable[fd]) {
        CPUContext->rax = (U64)(-1); // Bad FD
        return;
    }

    File *FileDescriptor = Curtask->FDTable[fd];
    
    // Decrement RefCount
    if (FileDescriptor->RefCount > 0) {
        FileDescriptor->RefCount--;
    }

    if (FileDescriptor->RefCount == 0) {
        VFSManager::Close(FileDescriptor);
    }
    
    Curtask->FDTable[fd] = nullptr;

    CPUContext->rax = 0; // Success
}

VOID Sys_Dup2(CpuContext_T *CPUContext){
    U64 OldFD = CPUContext->rdi;
    U64 NewFD = CPUContext->rsi;

    Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();
    if (!Curtask) {
        CPUContext->rax = (U64)(-1); // Error
        return;
    }

    if(OldFD >= MAX_FILE_IN_PROCESS || NewFD >= MAX_FILE_IN_PROCESS ||
       !Curtask->FDTable[OldFD]) {
        CPUContext->rax = (U64)(-1); // Bad FD
        return;
    }

    if(OldFD == NewFD){
        CPUContext->rax = (U64)NewFD; // Success
        return;
    }

    if(Curtask->FDTable[NewFD]){
        File *ToClose = Curtask->FDTable[NewFD];
        // Decrement RefCount
        if (ToClose->RefCount > 0) {
            ToClose->RefCount--;
        }
        if(ToClose->RefCount == 0){
            VFSManager::Close(ToClose);
        }
        Curtask->FDTable[NewFD] = nullptr;
    }

    Curtask->FDTable[NewFD] = Curtask->FDTable[OldFD];
    // Increment RefCount
    Curtask->FDTable[NewFD]->RefCount++;

    CPUContext->rax = (U64)NewFD; // Success
}

VOID Sys_Getdents64(CpuContext_T *CPUContext){
    U64 fd = CPUContext->rdi;
    U64 dirp = CPUContext->rsi;
    U64 count = CPUContext->rdx;

    Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();
    if(!Curtask){
        CPUContext->rax = (U64)(-1);
        return;
    }

    if(fd >= MAX_FILE_IN_PROCESS || !Curtask->FDTable[fd]){
        CPUContext->rax = (U64)(-1); // Bad FD
        return;
    }

    File *f = Curtask->FDTable[fd];

    // Allocate kernel buffer to receive directory entries, then copy to user.
    if(count == 0){ CPUContext->rax = 0; return; }
    U32 userBufSize = (U32)count;
    // Use a larger kernel buffer to hold raw names; conversion to dirent expands size.
    const U32 KERNEL_NAME_BUF = 16 * 1024; // 16 KiB
    U32 kbufSize = (userBufSize > KERNEL_NAME_BUF) ? userBufSize : KERNEL_NAME_BUF;
    U8* kbuf = (U8*)Kmalloc::Alloc(kbufSize);
    if(!kbuf){ CPUContext->rax = (U64)(-1); return; }

    INTN n = VFSManager::ReadDir(f, (void*)kbuf, kbufSize);
    if(n < 0){ Kmalloc::Free(kbuf); CPUContext->rax = (U64)(-1); return; }

    // Convert NUL-separated name list (from ReadDir) into linux_dirent64 entries
    // linux_dirent64 layout:
    //   u64 d_ino; s64 d_off; unsigned short d_reclen; unsigned char d_type; char d_name[];

    // Build entries into an output buffer up to bufSize bytes
    U8* outbuf = (U8*)Kmalloc::Alloc(userBufSize);
    if(!outbuf){ Kmalloc::Free(kbuf); CPUContext->rax = (U64)(-1); return; }

    U8* outp = outbuf;
    U32 outRemain = userBufSize;
    U8* inp = kbuf;
    U64 d_off = 0;

    // Hitung offset di mana nama dimulai (biasanya 19 byte)
    // Kita pakai trik casting null pointer buat dapet offset tanpa library <cstddef>
    const U32 OFFSET_NAME = (U32)(U64)( &((linux_dirent64*)0)->d_name );

    while((U32)(inp - kbuf) < (U32)n){
        const char* name = (const char*)inp;
        U32 namelen = (U32)String::Strlen(name);
        
        if(namelen == 0) { inp++; continue; }

        // Hitung ukuran total: Header (sampai sebelum nama) + Nama + Null
        U32 baseSize = OFFSET_NAME + namelen + 1;
        
        // Align up to 8 bytes
        U32 reclen = (baseSize + 7) & ~7U;

        if(reclen > outRemain) break;

        // --- CARA ISI DATA YANG AMAN ---
        
        // 1. Cast pointer output ke struct
        struct linux_dirent64* d = (struct linux_dirent64*)outp;
        
        d->d_ino = 1; 
        d->d_off = d_off + 1;
        d->d_reclen = (U16)reclen;
        d->d_type = DT_UNKNOWN; 

        // 2. Copy nama file MANUAL ke posisi d_name
        // Kita nulis melampaui ukuran array [1], tapi ini aman karena 
        // kita menulis ke 'outbuf' yang sudah kita alokasikan besar.
        String::Memcpy((U8*)d->d_name, (const U8*)name, namelen);
        
        // 3. Pasang Null Terminator
        d->d_name[namelen] = '\0';

        // 4. Zero Padding sisanya
        U32 paddingStart = baseSize;
        U32 paddingLen = reclen - paddingStart;
        if(paddingLen > 0){
             String::Memset(outp + paddingStart, 0, paddingLen);
        }

        outp += reclen;
        outRemain -= reclen;
        inp += namelen + 1;
        d_off++;
    }

    U32 totalOut = userBufSize - outRemain;

    // Copy constructed dirents to user buffer using task's CR3
    U64 *user_pml4 = HHDM_PhysToVirt(Curtask->CR3);
    if(!PageAlloc::CopyToUser(user_pml4, (void*)dirp, outbuf, (U64)totalOut)){
        Kmalloc::Free(kbuf);
        Kmalloc::Free(outbuf);
        CPUContext->rax = (U64)(-1);
        return;
    }

    Kmalloc::Free(kbuf);
    Kmalloc::Free(outbuf);
    CPUContext->rax = (U64)totalOut;
}

// untuk CD
VOID CanonicalizePath(const char* cwd, const char* input, char* output) {
    char temp[256];
    
    // 1. Handle Absolute vs Relative
    if (input[0] == '/') {
        // Absolute path
        String::Strcpy(temp, input);
    } else {
        // Relative path: Gabung CWD + Input
        String::Strcpy(temp, cwd);
        int len = String::Strlen(temp);
        if (len > 0 && temp[len-1] != '/') String::Strcat(temp, "/");
        String::Strcat(temp, input);
    }

    // 2. Tokenize dan Rebuild
    // Kita pakai stack sederhana untuk handle ".."
    char* tokens[32]; // Max depth 32
    int top = 0;
    
    char work_buf[256];
    String::Strcpy(work_buf, temp);

    UNUSED__ char* context = nullptr;
    // Asumsi lu punya String::Strtok atau sejenisnya. 
    // Kalau pakai Tokenize lu yg di userland tadi, sesuaikan logicnya.
    // Disini saya pakai logic manual parsing "/" biar aman.
    
    int len = String::Strlen(work_buf);
    char* start = work_buf;
    
    for (int i = 0; i <= len; i++) {
        if (work_buf[i] == '/' || work_buf[i] == '\0') {
            work_buf[i] = '\0';
            if (String::Strlen(start) > 0) {
                if (String::Strcmp(start, ".") == 0) {
                    // Ignore "."
                } else if (String::Strcmp(start, "..") == 0) {
                    // Pop stack
                    if (top > 0) top--;
                } else {
                    // Push stack
                    tokens[top++] = start;
                }
            }
            start = &work_buf[i+1];
        }
    }

    // 3. Reconstruct Output
    if (top == 0) {
        String::Strcpy(output, "/");
        return;
    }

    output[0] = '\0';
    for (int i = 0; i < top; i++) {
        String::Strcat(output, "/");
        String::Strcat(output, tokens[i]);
    }
}

I64 IsDirectory(const char* path) {
    File* f = VFSManager::Open(path); // Atau VFSManager::GetNode(path)
    if (!f) return -ROS_NOTFOUND;
    
    if(f->IsDirectory == FALSE){
        VFSManager::Close(f); 
        return FALSE;
    }

    VFSManager::Close(f); 
    return TRUE;
}

VOID Sys_Chdir(CpuContext_T *CPUContext){
    const char *path = (const char *)CPUContext->rdi;

    // lebih safe ambil pake UserCopy
    CHAR8 UserPath[256];
    CHAR8 FinalPath[256];
    Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();
    if (!Curtask) {
        CPUContext->rax = (U64)(-1);
        return;
    }

    // Build user PML4 pointer from saved CR3 in the Task. CR3 must be
    // a valid physical frame (non-zero).
    if (Curtask->CR3 == 0) {
        CPUContext->rax = (U64)(-1);
        Printk::Write(Printk::Level::LOG_ERR, "Sys_Chdir: invalid CR3 (0) for current task\n");
        return;
    }
    U64 *user_pml4 = HHDM_PhysToVirt(Curtask->CR3);
    if (!PageAlloc::CopyFromUser(user_pml4, UserPath, (void*)path, sizeof(UserPath) - 1)) {
        CPUContext->rax = (U64)(-1);
        Printk::Write(Printk::Level::LOG_ERR, "Sys_Chdir: CopyFromUser failed (path=%p CR3=0x%llx)\n",
                      (void*)path, (unsigned long long)Curtask->CR3);
        return;
    }

    CanonicalizePath(Curtask->CWD, UserPath, FinalPath);

    switch(IsDirectory(FinalPath)){
        case (-ROS_NOTFOUND):
            CPUContext->rax = (U64)(-2);
            return;
        case FALSE:
            CPUContext->rax = (U64)(-20);
            return;
        default:
            break;
    }

    String::Strcpy(Curtask->CWD, FinalPath);

    CPUContext->rax = 0; // Success
}

VOID Sys_GetCWD(CpuContext_T *CPUContext){
    CHAR8* buf = (CHAR8*)CPUContext->rdi;
    U64 size = CPUContext->rsi;

    Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();
    if (!Curtask) {
        CPUContext->rax = (U64)(-1);
        return;
    }

    U64 cwdLen = String::Strlen(Curtask->CWD);
    if (size < cwdLen + 1) {
        CPUContext->rax = (U64)(-1); // Buffer too small
        return;
    }

    // Copy to user
    U64 *user_pml4 = HHDM_PhysToVirt(Curtask->CR3);
    if (!PageAlloc::CopyToUser(user_pml4, (void*)buf, Curtask->CWD, cwdLen + 1)) {
        CPUContext->rax = (U64)(-1);
        return;
    }

    CPUContext->rax = (U64)cwdLen; // Return length of CWD
}

VOID Sys_Pipe(CpuContext_T *CPUContext){
    U64 Pipefd_Ptr = CPUContext->rdi;

    Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();

    INTN FDRead = -1, FDWrite = -1;
    for(INTN i = 0; i < MAX_FILE_IN_PROCESS; i++){
        if(Curtask->FDTable[i] == nullptr){
            if(FDRead == -1) FDRead = i;
            else {FDWrite = i; break; }
        }
    }

    if(FDRead == -1 || FDWrite == -1){
        CPUContext->rax = -1;
        return;
    }

    PipeBuffer *Buf = new PipeBuffer();
    Buf->ReadPos = 0;
    Buf->WritePos = 0;
    Buf->BytesAvailable = 0;
    Buf->IsWriteClosed = FALSE;
    Buf->RefCount = 2; // untuk read dan write end

    PipeFile *FRead = new PipeFile(Buf, FALSE);
    PipeFile *FWrite = new PipeFile(Buf, TRUE);

    FileSystem* pipeDriver = PipeFileSystem::GetInstance();
    FRead->FSOwner = pipeDriver;
    FWrite->FSOwner = pipeDriver;

    Curtask->FDTable[FDRead] = FRead;
    Curtask->FDTable[FDWrite] = FWrite;

    INTN FDS[2] = {FDRead, FDWrite};
    U64 *user_pml4 = HHDM_PhysToVirt(Curtask->CR3);
    if (!PageAlloc::CopyToUser(user_pml4, (void*)Pipefd_Ptr, (void*)FDS, sizeof(FDS))) {
        // Cleanup on failure
        Curtask->FDTable[FDRead] = nullptr;
        Curtask->FDTable[FDWrite] = nullptr;
        delete FRead;
        delete FWrite;
        delete Buf;
        CPUContext->rax = -1;
        return;
    }

    CPUContext->rax = 0; // Success
}

VOID Sys_Ioctl(CpuContext_T *CPUContext){
    U64 fd = CPUContext->rdi;
    U64 request = CPUContext->rsi;
    U64 argp = CPUContext->rdx;

    Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();
    if (!Curtask) {
        CPUContext->rax = (U64)(-1); // Error
        return;
    }

    if (fd >= MAX_FILE_IN_PROCESS || !Curtask->FDTable[fd]) {
        CPUContext->rax = (U64)(-1); // Bad file descriptor
        return;
    }

    File *FileDescriptor = Curtask->FDTable[fd];
    //Printk::Write(Printk::Level::LOG_INFO, "Sys_Ioctl: fd=%llu request=0x%llx argp=0x%llx\n",
    //              (unsigned long long)fd,
    //              (unsigned long long)request,
    //              (unsigned long long)argp);
    INTN Result = VFSManager::Ioctl(FileDescriptor, (U32)request, argp);
    if (Result == -1) {
        CPUContext->rax = (U64)(-1); // Error
        return;
    }

    CPUContext->rax = 0; // Success
}

VOID Sys_Sync(CpuContext_T *CPUContext){
    // Flush semua filesystem yang ter-mount
    VFSManager::SyncAll();

    CPUContext->rax = 0; // Success
}