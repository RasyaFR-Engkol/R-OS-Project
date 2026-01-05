#pragma once
#include "rosval.h"
#include <stdarg.h>

namespace String{
	// Memory
	void* Memcpy(void* dst, const void* src, unsigned long long n);
	void* Memmove(void* dst, const void* src, unsigned long long n);
	void* Memset(void* dst, int value, unsigned long long n);
	int   Memcmp(const void* a, const void* b, unsigned long long n);

	// Memory (wide writes) — useful for framebuffers/page tables
	unsigned short*  Memset16(unsigned short* dst, unsigned short value, unsigned long long count);
	unsigned int*    Memset32(unsigned int* dst, unsigned int value, unsigned long long count);
	unsigned long long* Memset64(unsigned long long* dst, unsigned long long value, unsigned long long count);

	// Strings (C-style, null-terminated)
	unsigned long long Strlen(const char* s);
	unsigned long long Strnlen(const char* s, unsigned long long maxlen);
	int   Strcmp(const char* a, const char* b);
	int   Strncmp(const char* a, const char* b, unsigned long long n);
	char* Strcpy(char* dst, const char* src);
	char* Strncpy(char* dst, const char* src, unsigned long long n);
	char* Strcat(char* dst, const char* src);
	const char* Strchr(const char* s, int ch);
	const char* Strrchr(const char* s, int ch);
	const char* Strstr(const char* haystack, const char* needle);

	// Tokenize string using delimiters. Behaves like C's strtok: on first call
	// pass the string to tokenize in `s`, subsequent calls pass `nullptr` to
	// continue tokenizing the same string. Not reentrant.
	char* Strtok(char* s, const char* delim);

	// Simple integer to string (no allocation, returns buffer)
	// Base supported: 2..16. Buffer must be large enough.
	// Returns pointer to buffer for convenience.
	char* Utoa(unsigned long long value, char* buffer, int base);
	char* Itoa(long long value, char* buffer, int base);

	// Variadic-safe sprintf variant that writes into a provided buffer.
	// - buffer: destination buffer
	// - bufsize: size of destination buffer in bytes
	// - fmt: format string
	// - args: VA_LIST of arguments (use VA_START/VA_END by caller)
	// Returns number of characters written (excluding trailing NUL). If bufsize>0
	// the result is NUL-terminated.
	int VSPrint(char* buffer, unsigned long long bufsize, const char* fmt, va_list args);

	// Convenience variadic wrapper around VSPrint (like sprintf but bounded).
	int SPrint(char* buffer, unsigned long long bufsize, const char* fmt, ...);
	void SplitPath(const char* fullPath, char* outParent, char* outName);
	inline U32 Min(U32 a, U32 b) { return (a < b) ? a : b; }
}