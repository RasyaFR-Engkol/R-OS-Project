#include <rosval.h>
#include <rossys.hpp>
#include <mm.hpp>
#include <cpu_context.hpp>
#include <filesystem/filesystem.hpp>
#include <task.hpp>
#include <filesystem/linux_dirent.hpp>
#include <ros_linux/kernelstat.hpp>
#include "syscall/sysarg.hpp"

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
        CPUContext->rax = 0;
        return;
    }

    Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();
    if (!Curtask) {
        Kmalloc::Free(Buffer);
        CPUContext->rax = (U64)(-1);
        return;
    }
    
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

VOID Sys_Seek(CpuContext_T *CPUContext){
    // 1. Ambil Argumen dari Register
    U64 fd = CPUContext->rdi;          // Arg 1: File Descriptor
    U64 offset = CPUContext->rsi;      // Arg 2: Offset
    U32 origin = (U32)CPUContext->rdx; // Arg 3: Origin (SEEK_SET/CUR/END)

    // 2. Validasi Task dan FD Table (Sama kayak Sys_Close)
    Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();
    
    // Cek batas MAX_FILE dan apakah slot FD tersebut kosong
    if (!Curtask || fd >= MAX_FILE_IN_PROCESS || !Curtask->FDTable[fd]) {
        CPUContext->rax = (U64)(-1); // EBADF (Bad File Descriptor)
        return;
    }

    File *FileDescriptor = Curtask->FDTable[fd];
    BOOL result = VFSManager::Seek(FileDescriptor, offset, origin);

    if (result == FALSE) {
        CPUContext->rax = (U64)(-1); // EINVAL atau ESPIPE
    } else {
        CPUContext->rax = FileDescriptor->CurrentPosition; 
    }
}

VOID Sys_Open(CpuContext_T *CPUContext){
    U64 pathname_ptr =CPUContext->rdi;
    U64 flags = CPUContext->rsi;
    __MAYBE_UNUSED U64 mode = CPUContext->rdx;

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

    File *OpenedFile = VFSManager::Open((const char*)FinalPath, flags);
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
    CPUContext->rax = (U64)FdIDX; // Success
}

VOID Sys_Close(CpuContext_T *CPUContext){
    U64 fd = CPUContext->rdi;

    Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();
    if (!Curtask || fd >= MAX_FILE_IN_PROCESS || !Curtask->FDTable[fd]) {
        CPUContext->rax = -ROS_ERROR_BAD_FD;
        Printk::Write(Printk::Level::LOG_DERR, "BAD FD, NO CURTASK, MAXIMAL FD.\n");
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

    CPUContext->rax = ROS_OK; // Success
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

I64 IsDirectory(const char* path) {
    File* f = VFSManager::Open(path, O_RDONLY); // Atau VFSManager::GetNode(path)
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

VOID Sys_Stat(CpuContext_T *CPUContext){
    CONSTANT CHAR8 *UserPath = (CONSTANT CHAR8*) CATCHARG1(CPUContext);
    struct kernel_stat *UserStatBuf = (struct kernel_stat*) CATCHARG2(CPUContext);

    Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();

    CHAR8 KernelPath[256];
    U64 *user_pml4 = HHDM_PhysToVirt(Curtask->CR3);
    UNUSED__ BOOL copy_success = false;
    for (int i = 0; i < (int)sizeof(KernelPath) - 1; i++) {
        char c;
        // Copy 1 byte saja dari user
        if (!PageAlloc::CopyFromUser(user_pml4, &c, (void*)(UserPath + i), 1)) {
            // Kalau gagal baca di tengah jalan, berarti segfault
            RETVAL(CPUContext) = -ROS_ERROR_FAULTY_ADDRESS;
            Printk::Write(Printk::Level::LOG_ERR, "Sys_Open: Segfault reading path at offset %d\n", i);
            return;
        }

        KernelPath[i] = c;
        
        // Kalau ketemu null terminator, stop! Kita sudah dapat stringnya.
        if (c == '\0') {
            copy_success = true;
            break;
        }
    }

    FileSystem *FS_PTR = nullptr;
    CHAR8 RelativePath[256];

    if(!VFSManager::ResolvePath(KernelPath, &FS_PTR, RelativePath)){
        RETVAL(CPUContext) = -ROS_ERROR_NO_ENTRY;
        return;
    }

    FileInfo info;
    if(FS_PTR->Stat(RelativePath, &info) != 0){
        RETVAL(CPUContext) = -ROS_ERROR_NO_ENTRY;
        return;
    }

    struct kernel_stat kstat;
    String::Memset(&kstat, 0, sizeof(kstat));

    kstat.st_size = info.Size;
    kstat.st_ino = info.InodeID;

    U32 mode_type = 0;
    U32 mode_perm = 0644; // Default rw-r--r--

    switch(info.Type) {
        case FT_DIR:
            mode_type = 0040000; // S_IFDIR (Directory)
            mode_perm = 0755;    // Biasanya rwxr-xr-x
            break;

        case FT_DEVCHAR:
            mode_type = 0020000; // S_IFCHR (Character Device, misal /dev/tty)
            mode_perm = 0600;    // Biasanya root only
            break;

        case FT_DEVBLOK:
            mode_type = 0060000; // S_IFBLK (Block Device, misal /dev/sda)
            mode_perm = 0600;
            break;

        case FT_PIPE:
            mode_type = 0010000; // S_IFIFO (Named Pipe / FIFO)
            mode_perm = 0666;
            break;

        case FT_SOCK:
            mode_type = 0140000; // S_IFSOCK (Socket file, misal /var/run/docker.sock)
            mode_perm = 0777;
            break;

        case FT_SYMLINK:
            // Sys_Stat biasanya nge-follow link, jadi harusnya info.Type
            // udah berubah jadi target aslinya.
            // TAPI, kalau link-nya broken (target gak ada),
            // ResolvePath biasanya tetep return info symlink itu sendiri (tergantung implementasi).
            mode_type = 0120000; // S_IFLNK
            mode_perm = 0777;
            break;

        case FT_NORMAL:
        default:
            mode_type = 0100000; // S_IFREG (Regular File)
            break;
    }

    kstat.st_mode = mode_type | mode_perm;

    if(!PageAlloc::CopyToUser(user_pml4, UserStatBuf, &kstat, sizeof(kernel_stat))){
        RETVAL(CPUContext) = -ROS_ERROR_FAULTY_ADDRESS;
        return;
    }

    RETVAL(CPUContext) = 0;
}

VOID Sys_Fstat(CpuContext_T *CPUContext){
    // Argument 1: File Descriptor (int)
    I64 fd = (I64) CATCHARG1(CPUContext); 
    // Argument 2: Struct Stat Buffer
    struct kernel_stat *UserStatBuf = (struct kernel_stat*) CATCHARG2(CPUContext);

    Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();
    
    // 1. Validasi File Descriptor
    // Cek range dan apakah slotnya kosong
    if(fd < 0 || fd >= MAX_FILE_IN_PROCESS || Curtask->FDTable[fd] == nullptr){
        RETVAL(CPUContext) = -ROS_ERROR_BAD_FD;
        return;
    }

    // 2. Ambil Pointer File yang udah ada di Memory
    File *Handle = Curtask->FDTable[fd];

    // 3. Isi struct kernel_stat langsung dari File Handle
    struct kernel_stat kstat;
    String::Memset(&kstat, 0, sizeof(kstat));

    kstat.st_size = Handle->FileSize;
    kstat.st_ino  = Handle->InodeID; // Pastikan struct File udah diupdate

    // 4. Tentukan Mode (File Biasa, Folder, atau Pipe/Socket?)
    // Ini penting biar 'ls' atau shell tau cara treat fd ini.
    
    // Default permission bits
    U32 mode_perm = 0644; 
    U32 mode_type = 0;

    switch(Handle->type){ // atau info.Type
        case FT_DIR:
            mode_type = 0040000; // S_IFDIR
            mode_perm = 0755;    // Folder biasanya rwx
            break;

        case FT_NORMAL:
        case FT_SHM: // SHM diperlakukan sbg file biasa (tmpfs)
            mode_type = 0100000; // S_IFREG
            break;

        case FT_SOCK:
            mode_type = 0140000; // S_IFSOCK
            break;

        case FT_SYMLINK:
            mode_type = 0120000; // S_IFLNK
            mode_perm = 0777;    // Symlink biasanya 777 (permission ikut target)
            break;

        case FT_DEVBLOK:
            mode_type = 0060000; // S_IFBLK
            break;

        case FT_DEVCHAR:
            mode_type = 0020000; // S_IFCHR
            break;

        case FT_PIPE:
            mode_type = 0010000; // S_IFIFO
            break;
            
        default:
            // Fallback ke file biasa kalau tipe aneh
            mode_type = 0100000; 
            break;
    }

    kstat.st_mode = mode_type | mode_perm;

    // 5. Copy ke User Memory
    U64 *user_pml4 = HHDM_PhysToVirt(Curtask->CR3);
    if(!PageAlloc::CopyToUser(user_pml4, UserStatBuf, &kstat, sizeof(kernel_stat))){
        RETVAL(CPUContext) = -ROS_ERROR_FAULTY_ADDRESS;
        return;
    }

    RETVAL(CPUContext) = 0; // Success
}

VOID Sys_Lstat(CpuContext_T *CPUContext){
    CONSTANT CHAR8 *UserPath = (CONSTANT CHAR8*) CATCHARG1(CPUContext);
    struct kernel_stat *UserStatBuf = (struct kernel_stat*) CATCHARG2(CPUContext);

    Tasking::Task *Curtask = Tasking::GetCurrentTaskPtr();

    CHAR8 KernelPath[256];
    U64 *user_pml4 = HHDM_PhysToVirt(Curtask->CR3);
    UNUSED__ BOOL copy_success = false;
    for (int i = 0; i < (int)sizeof(KernelPath) - 1; i++) {
        char c;
        // Copy 1 byte saja dari user
        if (!PageAlloc::CopyFromUser(user_pml4, &c, (void*)(UserPath + i), 1)) {
            // Kalau gagal baca di tengah jalan, berarti segfault
            RETVAL(CPUContext) = -ROS_ERROR_FAULTY_ADDRESS;
            Printk::Write(Printk::Level::LOG_ERR, "Sys_Open: Segfault reading path at offset %d\n", i);
            return;
        }

        KernelPath[i] = c;
        
        // Kalau ketemu null terminator, stop! Kita sudah dapat stringnya.
        if (c == '\0') {
            copy_success = true;
            break;
        }
    }

    FileSystem *FS_PTR = nullptr;
    CHAR8 RelativePath[256];

    if(!VFSManager::ResolvePath(KernelPath, &FS_PTR, RelativePath, FALSE)){
        RETVAL(CPUContext) = -ROS_ERROR_NO_ENTRY;
        return;
    }

    FileInfo info;
    if(FS_PTR->Stat(RelativePath, &info) != 0){
        RETVAL(CPUContext) = -ROS_ERROR_NO_ENTRY;
        return;
    }

    struct kernel_stat kstat;
    String::Memset(&kstat, 0, sizeof(kstat));

    if (info.Type == FT_SYMLINK) {
        kstat.st_mode = 0120000 | 0777; // <--- INI PENTING
    } 
    else if (info.IsDirectory || info.Type == FT_DIR) {
        kstat.st_mode = 0040000 | 0755; 
    } 
    else {
        kstat.st_mode = 0100000 | 0644; 
    }

    if(!PageAlloc::CopyToUser(user_pml4, UserStatBuf, &kstat, sizeof(kernel_stat))){
        RETVAL(CPUContext) = -ROS_ERROR_FAULTY_ADDRESS;
        return;
    }

    RETVAL(CPUContext) = 0;
}
