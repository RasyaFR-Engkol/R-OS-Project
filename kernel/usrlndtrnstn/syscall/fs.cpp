#include <rosval.h>
#include <rossys.hpp>
#include <mm.hpp>
#include <cpu_context.hpp>
#include <filesystem/filesystem.hpp>

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
    UNUSED__ U64 buf = CPUContext->rsi;
    U64 count = CPUContext->rdx;

    VOID *Buffer = Kmalloc::Alloc(count);
    if(!Buffer){
        CPUContext->rax = (U64)(-1); // Error
        return;
    }

    // Asumsi udah kebuka oleh sys_open
    File *FileDescriptor = (File*)fd;

    SIZE_T ReadBytes = VFSManager::Read(FileDescriptor, (U8*)Buffer, count);
    if(ReadBytes == (SIZE_T)(-1)){
        Kmalloc::Free(Buffer);
        CPUContext->rax = (U64)(-1); // Error
        return;
    }

    // Copy ke user buffer
    // Build user PML4 pointer from saved CR3 in the CPU context. CR3 is
    // expected to contain the physical CR3 for the user address space.
    //U64 *user_pml4 = HHDM_PhysToVirt((UPTR)CPUContext->cr3);

   // if (!PageAlloc::CopyToUser(user_pml4, (void*)buf, Buffer, ReadBytes)) {
     //   Kmalloc::Free(Buffer);
       // CPUContext->rax = (U64)(-1);
       // return;
    // }

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

    // Copy dari user buffer
    // Build user PML4 pointer from saved CR3 in the CPU context. CR3 is
    // expected to contain the physical CR3 for the user address space.
    //U64 *user_pml4 = HHDM_PhysToVirt((UPTR)CPUContext->cr3);

    //if (!PageAlloc::CopyFromUser(user_pml4, Buffer, (void*)buf, count)) {
   //     Kmalloc::Free(Buffer);
    //    CPUContext->rax = (U64)(-1);
    //    return;
   // }

    // Asumsi udah kebuka oleh sys_open
    File *FileDecsriptor = (File*)fd;

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
   UNUSED__  U64 pathname_ptr =CPUContext->rdi;
    UNUSED__ U64 flags = CPUContext->rsi;
    UNUSED__ U64 mode = CPUContext->rdx;

    // Copy pathname dari user
    CHAR8 PathName[256];
    String::Memset(PathName, 0, sizeof(PathName));
    // Build user PML4 pointer from saved CR3 in the CPU context. CR3 is
    // expected to contain the physical CR3 for the user address space.
   // U64 *user_pml4 = HHDM_PhysToVirt((UPTR)CPUContext->cr3);

  //  if(!PageAlloc::CopyFromUser(user_pml4, PathName, (VOID*)pathname_ptr, sizeof(PathName))){
   //     CPUContext->rax = (U64)(-1); // Error
   ////     return;
  //  }

    File *OpenedFile = VFSManager::Open(PathName);
    if(!OpenedFile){
        CPUContext->rax = (U64)(-1); // Error
        return;
    }

    CPUContext->rax = (U64)OpenedFile;
}

VOID Sys_Close(CpuContext_T *CPUContext){
    U64 fd = CPUContext->rdi;

    File *FileDescriptor = (File*)fd;

    VFSManager::Close(FileDescriptor);

    CPUContext->rax = 0; // Success
}   