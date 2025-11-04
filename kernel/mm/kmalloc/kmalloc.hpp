#pragma once

#include <rosval.h>

namespace Kmalloc {
    // Initialize kernel heap with 'pages' number of pages (each PAGE_SIZE)
    // Allocates virtual pages from PageAlloc::Virtual and maps physical pages
    // into them so the heap is usable immediately.
    void Init(SIZE_T pages = 16);

    // Allocate 'size' bytes from kernel heap. Returns nullptr on OOM.
    void* Alloc(SIZE_T size);

    // Free a previously allocated block.
    void Free(void* ptr);
}

// C-style aliases
static inline void kmalloc_init(size_t pages) { Kmalloc::Init((SIZE_T)pages); }
static inline void* kmalloc(size_t s) { return Kmalloc::Alloc((SIZE_T)s); }
static inline void kfree(void* p) { Kmalloc::Free(p); }
