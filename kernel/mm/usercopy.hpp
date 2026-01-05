#pragma once

#include "mm.hpp"

namespace PageAlloc {
    // Copy `len` bytes from user virtual address `srcUser` (in the address
    // space described by `UserPML4`) into kernel buffer `dstKernel`.
    // Returns TRUE on success, FALSE if any user page is not present or not
    // user-accessible.
    BOOL CopyFromUser(U64 *UserPML4, void* dstKernel, const void* srcUser, SIZE_T len);

    // Copy `len` bytes from kernel buffer `srcKernel` into user virtual
    // address `dstUser` (in the address space described by `UserPML4`).
    // Returns TRUE on success, FALSE on fault / invalid user pages.
    BOOL CopyToUser(U64 *UserPML4, void* dstUser, const void* srcKernel, SIZE_T len);

    // Diagnostic: dump the page-table entries for `vaddr` in the given
    // `UserPML4` and return FALSE if the address is not mappable for user
    // access. This helps debug CopyFromUser failures.
    BOOL DumpVaddrMapping(U64 *UserPML4, UPTR vaddr);
    // Dump mapping for the whole range [vaddr, vaddr+len). Useful to find
    // which page inside a multi-page copy is missing or not user-accessible.
    BOOL DumpVaddrRange(U64 *UserPML4, UPTR vaddr, SIZE_T len);
}
