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

// ---------------------- Tokenization ----------------------
// Simple strtok implementation (not reentrant). Modifies the input string by
// replacing delimiter characters with '\0' and returns pointers to tokens.
char* Strtok(char* s, const char* delim) {
    static char* next = nullptr;
    if (!delim) return nullptr;
    // If s is null, continue from previous position
    if (s == nullptr) s = next;
    if (s == nullptr) return nullptr;

    // Skip leading delimiters
    char* p = s;
    bool any = true;
    while (*p != '\0') {
        // check if *p is one of delimiters
        const char* d = delim;
        any = false;
        while (*d) {
            if (*p == *d) { any = true; break; }
            ++d;
        }
        if (!any) break;
        ++p;
    }
    if (*p == '\0') { next = nullptr; return nullptr; }

    // p now at start of token
    char* token = p;

    // Find end of token
    while (*p != '\0') {
        const char* d = delim;
        bool is_del = false;
        while (*d) {
            if (*p == *d) { is_del = true; break; }
            ++d;
        }
        if (is_del) {
            *p = '\0';
            next = p + 1;
            return token;
        }
        ++p;
    }

    // Reached end of string
    next = nullptr;
    return token;
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

namespace String {
// VSPrint implementation: bounded formatting into caller-provided buffer.
int VSPrint(char* buffer, unsigned long long bufsize, const char* fmt, va_list args) {
    if (!buffer || bufsize == 0) {
        // We still compute the would-be length but cannot store characters.
        // For simplicity return 0 when buffer is null or size is 0.
        return 0;
    }

    unsigned long long pos = 0;

    auto writeChar = [&](char c) {
        if (pos + 1 < bufsize) buffer[pos] = c;
        pos++;
    };
    auto writeStr = [&](const char* s, unsigned long long len) {
        if (!s) return;
        for (unsigned long long i = 0; i < len; ++i) writeChar(s[i]);
    };
    auto writeNulTerminate = [&]() {
        unsigned long long idx = (pos < (bufsize ? bufsize - 1 : 0)) ? pos : (bufsize ? bufsize - 1 : 0);
        buffer[idx] = '\0';
    };

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;

            BOOL ZeroPadding = FALSE;
            BOOL LeftJustify = FALSE;
            if (*fmt == '-') { LeftJustify = TRUE; fmt++; }
            if (*fmt == '0') { ZeroPadding = TRUE; fmt++; }

            VAL32 Width = 0;
            while (*fmt >= '0' && *fmt <= '9') { Width = Width * 10 + (*fmt - '0'); fmt++; }

            BOOL HasPrecision = FALSE;
            VAL32 Precision = 0;
            if (*fmt == '.') {
                fmt++; HasPrecision = TRUE;
                while (*fmt >= '0' && *fmt <= '9') { Precision = Precision * 10 + (*fmt - '0'); fmt++; }
            }

            enum LenMod { LM_NONE, LM_L, LM_LL, LM_Z } LM = LM_NONE;
            if (*fmt == 'l') { fmt++; if (*fmt == 'l') { LM = LM_LL; fmt++; } else LM = LM_L; }
            else if (*fmt == 'z') { LM = LM_Z; fmt++; }

            CHAR8 BufLocal[128];

            switch (*fmt) {
                case '%': { writeChar('%'); break; }
                case 's': {
                    const CHAR8* s = va_arg(args, const CHAR8*);
                    if (!s) s = "(null)";
                    unsigned long long slen = Strlen(s);
                    if (HasPrecision && Precision < (VAL32)slen) slen = Precision;
                    int pad = (Width > (int)slen) ? (Width - (int)slen) : 0;
                    if (!LeftJustify) for (int i=0;i<pad;++i) writeChar(' ');
                    writeStr(s, slen);
                    if (LeftJustify) for (int i=0;i<pad;++i) writeChar(' ');
                    break;
                }
                case 'c': {
                    int ch = va_arg(args, int);
                    writeChar((char)ch);
                    break;
                }
                case 'd': case 'i': {
                    long long sval = 0;
                    if (LM == LM_LL) sval = va_arg(args, long long);
                    else if (LM == LM_L) sval = (long long)va_arg(args, long);
                    else if (LM == LM_Z) sval = (long long)va_arg(args, size_t);
                    else sval = (long long)va_arg(args, int);
                    Itoa(sval, BufLocal, 10);
                    char* out = BufLocal;
                    bool neg = (out[0] == '-'); if (neg) ++out;
                    unsigned long long len = Strlen(out);
                    int num_digits = (int)len;
                    int total_len = num_digits + (neg ? 1 : 0);
                    int min_digits = HasPrecision ? (int)Precision : 0;
                    int zero_pad = (min_digits > num_digits) ? (min_digits - num_digits) : 0;
                    if (!HasPrecision && ZeroPadding && !LeftJustify && Width > total_len) zero_pad = Width - total_len;
                    if (LeftJustify) {
                        if (neg) writeChar('-');
                        for (int z=0; z<zero_pad; ++z) writeChar('0');
                        writeStr(out, len);
                        for (int i = total_len + zero_pad; i < Width; ++i) writeChar(' ');
                    } else {
                        if (zero_pad == 0) {
                            for (int i = total_len; i < Width; ++i) writeChar(' ');
                            if (neg) writeChar('-');
                        } else {
                            if (neg) writeChar('-');
                            for (int z=0; z<zero_pad; ++z) writeChar('0');
                        }
                        writeStr(out, len);
                    }
                    break;
                }
                case 'u': {
                    unsigned long long uval = 0ULL;
                    if (LM == LM_LL) uval = va_arg(args, unsigned long long);
                    else if (LM == LM_L) uval = (unsigned long long)va_arg(args, unsigned long);
                    else if (LM == LM_Z) uval = (unsigned long long)va_arg(args, size_t);
                    else uval = (unsigned long long)va_arg(args, unsigned int);
                    Utoa(uval, BufLocal, 10);
                    unsigned long long len = Strlen(BufLocal);
                    int num_digits = (int)len;
                    int min_digits = HasPrecision ? (int)Precision : 0;
                    int zero_pad = (min_digits > num_digits) ? (min_digits - num_digits) : 0;
                    if (!HasPrecision && ZeroPadding && !LeftJustify && Width > num_digits) zero_pad = Width - num_digits;
                    if (LeftJustify) {
                        for (int z=0; z<zero_pad; ++z) writeChar('0');
                        writeStr(BufLocal, len);
                        for (int i = num_digits + zero_pad; i < Width; ++i) writeChar(' ');
                    } else {
                        for (int i = num_digits + zero_pad; i < Width; ++i) writeChar(' ');
                        for (int z = 0; z < zero_pad; ++z) writeChar('0');
                        writeStr(BufLocal, len);
                    }
                    break;
                }
                case 'x': case 'X': {
                    unsigned long long uval = 0ULL;
                    if (LM == LM_LL) uval = va_arg(args, unsigned long long);
                    else if (LM == LM_L) uval = (unsigned long long)va_arg(args, unsigned long);
                    else if (LM == LM_Z) uval = (unsigned long long)va_arg(args, size_t);
                    else uval = (unsigned long long)va_arg(args, unsigned int);
                    Utoa(uval, BufLocal, 16);
                    if (*fmt == 'X') {
                        for (char* p = BufLocal; *p; ++p) if (*p >= 'a' && *p <= 'f') *p = *p - 'a' + 'A';
                    }
                    unsigned long long len = Strlen(BufLocal);
                    int num_digits = (int)len;
                    int min_digits = HasPrecision ? (int)Precision : 0;
                    int zero_pad = (min_digits > num_digits) ? (min_digits - num_digits) : 0;
                    if (!HasPrecision && ZeroPadding && !LeftJustify && Width > num_digits) zero_pad = Width - num_digits;
                    if (LeftJustify) {
                        for (int z=0; z<zero_pad; ++z) writeChar('0');
                        writeStr(BufLocal, len);
                        for (int i = num_digits + zero_pad; i < Width; ++i) writeChar(' ');
                    } else {
                        for (int i = num_digits + zero_pad; i < Width; ++i) writeChar(' ');
                        for (int z=0; z<zero_pad; ++z) writeChar('0');
                        writeStr(BufLocal, len);
                    }
                    break;
                }
                case 'f': {
#if defined(__SSE__) || defined(__AVX__) || defined(__SSE2__)
                    double fval = va_arg(args, double);
                    bool neg = false;
                    if (fval < 0.0) { neg = true; fval = -fval; }
                    int prec = HasPrecision ? (int)Precision : 6;
                    if (prec < 0) prec = 6;
                    long long intpart = (long long)fval;
                    double frac = fval - (double)intpart;
                    unsigned long long mul = 1ULL;
                    for (int i=0;i<prec;++i) mul *= 10ULL;
                    unsigned long long frac_digits = 0ULL;
                    {
                        double tmp = frac * (double)mul;
                        unsigned long long rounded = (unsigned long long)(tmp + 0.5);
                        if (rounded >= mul) { intpart += 1; rounded -= mul; }
                        frac_digits = rounded;
                    }
                    CHAR8 intbuf[48]; Utoa((unsigned long long)intpart, intbuf, 10);
                    CHAR8 fracbuf[64];
                    if (prec > 0) {
                        for (int i = prec - 1; i >= 0; --i) { fracbuf[i] = (CHAR8)('0' + (frac_digits % 10ULL)); frac_digits /= 10ULL; }
                        fracbuf[prec] = '\0';
                    } else {
                        fracbuf[0] = '\0';
                    }
                    CHAR8 whole[128]; size_t ppos = 0;
                    if (neg) whole[ppos++] = '-';
                    for (size_t i=0; intbuf[i]; ++i) whole[ppos++] = intbuf[i];
                    if (prec > 0) { whole[ppos++] = '.'; for (int i=0;i<prec;++i) whole[ppos++] = fracbuf[i]; }
                    whole[ppos] = '\0';
                    unsigned long long wlen = Strlen(whole);
                    int pad = (Width > (int)wlen) ? (Width - (int)wlen) : 0;
                    if (!LeftJustify) {
                        if (ZeroPadding && !HasPrecision) for (int i=0;i<pad;++i) writeChar('0');
                        else for (int i=0;i<pad;++i) writeChar(' ');
                    }
                    writeStr(whole, wlen);
                    if (LeftJustify) for (int i=0;i<pad;++i) writeChar(' ');
#else
                    /* floats not supported in this build: print placeholder */
                    const CHAR8 whole_fallback[] = "<float>";
                    unsigned long long wlen = Strlen(whole_fallback);
                    int pad = (Width > (int)wlen) ? (Width - (int)wlen) : 0;
                    if (!LeftJustify) for (int i=0;i<pad;++i) writeChar(' ');
                    writeStr(whole_fallback, wlen);
                    if (LeftJustify) for (int i=0;i<pad;++i) writeChar(' ');
#endif
                    break;
                }
                case 'p': {
                    void* ptr = va_arg(args, void*);
                    unsigned long long pv = (unsigned long long)ptr;
                    Utoa(pv, BufLocal, 16);
                    writeStr("0x", 2);
                    writeStr(BufLocal, Strlen(BufLocal));
                    break;
                }
                default: {
                    writeChar('%'); if (*fmt) writeChar(*fmt);
                    break;
                }
            }
        } else {
            writeChar(*fmt);
        }
        fmt++;
    }

    writeNulTerminate();
    // Return number of characters written (excluding trailing NUL)
    return (int)((pos < (bufsize ? bufsize - 1 : 0)) ? pos : (bufsize ? bufsize - 1 : 0));

}

int SPrint(char* buffer, unsigned long long bufsize, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int ret = VSPrint(buffer, bufsize, fmt, args);
    va_end(args);
    return ret;
}

void SplitPath(const char* fullPath, char* outParent, char* outName) {
    int len = String::Strlen(fullPath);
    int splitIdx = -1;

    int i = len;
    while(i > 0) {
        i--; // Geser ke kiri dulu (jadi len-1, len-2, ... 0)
        if(fullPath[i] == '/') {
            splitIdx = i;
            break;
        }
    }
    // -------------------------

    if(splitIdx == -1) { 
        // Gak ada slash, berarti file di root relative
        outParent[0] = '\0';
        String::Strcpy(outName, fullPath);
    } else {
        // Copy parent path
        // Note: Memcpy biasanya parameternya (dest, src, count)
        String::Memcpy((U8*)outParent, (const U8*)fullPath, splitIdx);
        outParent[splitIdx] = '\0';
        
        // Handle root case "/" (misal path: "/etc")
        // Kalau splitIdx 0, berarti parent-nya kosong string, tapi harusnya "/"
        if (splitIdx == 0) { 
            outParent[0] = '/'; 
            outParent[1] = '\0'; 
        }
        
        String::Strcpy(outName, fullPath + splitIdx + 1);
    }
}

} // namespace String

