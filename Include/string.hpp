#pragma once

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
	const char* Strchr(const char* s, int ch);
	const char* Strrchr(const char* s, int ch);
	const char* Strstr(const char* haystack, const char* needle);

	// Simple integer to string (no allocation, returns buffer)
	// Base supported: 2..16. Buffer must be large enough.
	// Returns pointer to buffer for convenience.
	char* Utoa(unsigned long long value, char* buffer, int base);
	char* Itoa(long long value, char* buffer, int base);
}