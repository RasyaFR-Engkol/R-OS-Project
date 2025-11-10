#include "../../Include/string.hpp"
#include "../../Include/rosval.h"
#include <stdint.h>
#include <stddef.h>

namespace String {

// ---------------------- Memory ----------------------
void* Memcpy(void* dst, const void* src, unsigned long long n) {
    // Undefined for overlap; caller must ensure non-overlapping regions.
    if (!n || dst == src) return dst;
    void* d = dst;
    const void* s = src;
    unsigned long long c = n;
    asm volatile(
        "cld; rep movsb"
        : "+D"(d), "+S"(s), "+c"(c)
        :
        : "memory", "cc");
    return dst;
}

void* Memmove(void* dst, const void* src, unsigned long long n) {
    if (dst == src || n == 0) return dst;
    auto* d = static_cast<unsigned char*>(dst);
    const auto* s = static_cast<const unsigned char*>(src);
    unsigned long long c = n;
    if (d < s) {
        void* D = d; const void* S = s; unsigned long long C = c;
        asm volatile(
            "cld; rep movsb"
            : "+D"(D), "+S"(S), "+c"(C)
            :
            : "memory", "cc");
    } else {
        // Backward copy for overlap
        if (c == 0) return dst;
        d += c - 1;
        s += c - 1;
        void* D = d; const void* S = s; unsigned long long C = c;
        asm volatile(
            "std; rep movsb; cld"
            : "+D"(D), "+S"(S), "+c"(C)
            :
            : "memory", "cc");
    }
    return dst;
}

void* Memset(void* dst, int value, unsigned long long n) {
    if (!n) return dst;
    void* d = dst;
    unsigned long long c = n;
    unsigned char v = static_cast<unsigned char>(value);
    asm volatile(
        "cld; rep stosb"
        : "+D"(d), "+c"(c)
        : "a"(v)
        : "memory", "cc");
    return dst;
}

int Memcmp(const void* a, const void* b, unsigned long long n) {
    const auto* x = static_cast<const unsigned char*>(a);
    const auto* y = static_cast<const unsigned char*>(b);
    for (unsigned long long i = 0; i < n; ++i) {
        if (x[i] != y[i]) return (x[i] < y[i]) ? -1 : 1;
    }
    return 0;
}

unsigned short* Memset16(unsigned short* dst, unsigned short value, unsigned long long count) {
    if (!count) return dst;
    unsigned long long c = count;
    void* d = dst;
    unsigned short v = value;
    asm volatile(
        "cld; rep stosw"
        : "+D"(d), "+c"(c)
        : "a"(v)
        : "memory", "cc");
    return dst;
}

unsigned int* Memset32(unsigned int* dst, unsigned int value, unsigned long long count) {
    if (!count) return dst;
    unsigned long long c = count;
    void* d = dst;
    unsigned int v = value;
    asm volatile(
        "cld; rep stosl"
        : "+D"(d), "+c"(c)
        : "a"(v)
        : "memory", "cc");
    return dst;
}

unsigned long long* Memset64(unsigned long long* dst, unsigned long long value, unsigned long long count) {
    if (!count) return dst;
    unsigned long long c = count;
    void* d = dst;
    unsigned long long v = value;
    asm volatile(
        "cld; rep stosq"
        : "+D"(d), "+c"(c)
        : "a"(v)
        : "memory", "cc");
    return dst;
}

// ---------------------- Strings ----------------------
unsigned long long Strlen(const char* s) {
    if (!s) return 0;
    const char* p = s;
    unsigned long long rc = ~0ULL;
    // Scan for NUL using repne scasb with AL=0, RCX=-1
    asm volatile(
        "xor %%eax, %%eax; cld; repne scasb"
        : "+c"(rc), "+D"(p)
        :
        : "rax", "memory", "cc");
    return (~rc) - 1ULL;
}

unsigned long long Strnlen(const char* s, unsigned long long maxlen) {
    if (!s) return 0;
    unsigned long long i = 0;
    while (i < maxlen && s[i] != '\0') ++i;
    return i;
}

int Strcmp(const char* a, const char* b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    const char* s = a;
    const char* d = b;
    int result;
    asm volatile(
        "cld\n\t"
        "1:\n\t"
        "lodsb\n\t"               // AL = *RSI++
        "scasb\n\t"               // compare AL with *RDI++
        "jne 2f\n\t"               // mismatch -> compute diff
        "test %%al, %%al\n\t"       // if AL==0 -> equal
        "jne 1b\n\t"
        "xor %%eax, %%eax\n\t"     // result = 0
        "jmp 3f\n\t"
        "2:\n\t"
        "movzbl -1(%1), %%edx\n\t"  // EDX = (unsigned char) *(RDI-1)
        "movzbl %%al, %%eax\n\t"     // EAX = (unsigned char) AL
        "sub %%edx, %%eax\n\t"       // EAX = a - b
        "3:"
        : "+S"(s), "+D"(d), "=a"(result)
        :
        : "edx", "memory", "cc");
    return result;
}

int Strncmp(const char* a, const char* b, unsigned long long n) {
    if (n == 0) return 0;
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    for (unsigned long long i = 0; i < n; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb) return ca - cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

char* Strcpy(char* dst, const char* src) {
    if (!dst) return nullptr;
    if (!src) { dst[0] = '\0'; return dst; }
    char* d = dst; const char* s = src;
    asm volatile(
        "cld\n\t"
        "1:\n\t"
        "lodsb\n\t"               // AL = *RSI++
        "stosb\n\t"               // *RDI++ = AL
        "test %%al, %%al\n\t"       // until NUL
        "jne 1b"
        : "+S"(s), "+D"(d)
        :
        : "eax", "memory", "cc");
    return dst;
}

char* Strncpy(char* dst, const char* src, unsigned long long n) {
    if (!dst || n == 0) return dst;
    unsigned long long i = 0;
    if (src) {
        for (; i < n && src[i] != '\0'; ++i) dst[i] = src[i];
    }
    for (; i < n; ++i) dst[i] = '\0';
    return dst;
}

char* Strcat(char* dst, const char* src) {
    if (!dst) return nullptr;
    if (!src) return dst;
    // Find end of destination string
    unsigned long long len = Strlen(dst);
    // Append source (including terminating NUL)
    Strcpy(dst + len, src);
    return dst;
}

const char* Strchr(const char* s, int ch) {
    if (!s) return nullptr;
    char c = (char)ch;
    for (; *s; ++s) if (*s == c) return s;
    return (c == '\0') ? s : nullptr;
}

const char* Strrchr(const char* s, int ch) {
    if (!s) return nullptr;
    const char* last = nullptr;
    char c = (char)ch;
    for (; *s; ++s) if (*s == c) last = s;
    return (c == '\0') ? s : last;
}

const char* Strstr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return nullptr;
    if (*needle == '\0') return haystack;
    unsigned long long nlen = Strlen(needle);
    for (const char* h = haystack; *h; ++h) {
        if (*h == *needle) {
            if (Strncmp(h, needle, nlen) == 0) return h;
        }
    }
    return nullptr;
}

// ---------------------- Integer to ASCII ----------------------
static char digit_of(unsigned v) {
    return (v < 10) ? (char)('0' + v) : (char)('a' + (v - 10));
}

char* Utoa(unsigned long long value, char* buffer, int base) {
    if (!buffer || base < 2 || base > 16) return buffer;
    char* p = buffer;
    // produce digits in reverse
    do {
        unsigned long long q = value / (unsigned)base;
        unsigned r = (unsigned)(value - q * (unsigned)base);
        *p++ = digit_of(r);
        value = q;
    } while (value != 0ULL);
    *p = '\0';
    // reverse in-place
    for (char *l = buffer, *r = p - 1; l < r; ++l, --r) {
        char tmp = *l; *l = *r; *r = tmp;
    }
    return buffer;
}

char* Itoa(long long value, char* buffer, int base) {
    if (!buffer || base < 2 || base > 16) return buffer;
    if (value == 0) { buffer[0] = '0'; buffer[1] = '\0'; return buffer; }
    bool neg = false;
    unsigned long long u;
    if (base == 10 && value < 0) { neg = true; u = (unsigned long long)(-value); }
    else { u = (unsigned long long)value; }
    char* p = buffer;
    do {
        unsigned long long q = u / (unsigned)base;
        unsigned r = (unsigned)(u - q * (unsigned)base);
        *p++ = digit_of(r);
        u = q;
    } while (u != 0ULL);
    if (neg) *p++ = '-';
    *p = '\0';
    // reverse
    for (char *l = buffer, *r = p - 1; l < r; ++l, --r) { char t = *l; *l = *r; *r = t; }
    return buffer;
}

} // namespace String
