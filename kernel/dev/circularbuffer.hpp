#pragma once

#include <rosval.h>

template <U32 Size>
class CircularBuffer{
    private:
        U8 Buffer[Size];
        VOLATILE U32 Head;
        VOLATILE U32 Tail;
    public:
        CircularBuffer() : Head(0), Tail(0) {}

        void Clear() {
            Head = 0;
            Tail = 0;
        }

        bool IsEmpty() const {
            return Head == Tail;
        }

        // Cek apakah penuh?
        bool IsFull() const {
            return ((Head + 1) % Size) == Tail;
        }

        // Masukin data (Dipanggil oleh Interrupt Handler / Keyboard Driver)
        bool Write(U8 data) {
            U32 nextHead = (Head + 1) % Size;

            if (nextHead == Tail) {
                return false; // Buffer Penuh, data kebuang (Overflow)
            }

            Buffer[Head] = data;
            Head = nextHead;
            return true;
        }

        // Ambil data (Dipanggil oleh Sys_Read)
        bool Read(U8* data) {
            if (Head == Tail) {
                return false; // Buffer Kosong
            }

            *data = Buffer[Tail];
            Tail = (Tail + 1) % Size;
            return true;
        }
        
        // Ngintip data tanpa ngambil (Opsional, kadang guna)
        bool Peek(U8* data) {
            if (Head == Tail) return false;
            *data = Buffer[Tail];
            return true;
        }
};