#pragma once

#include "spinlock/mutex.hpp"

class RwLock{
    private:
        Mutex ReaderLock;
        Mutex GlobalLock;
        INTN ReaderCount;

    public:
        RwLock() {
            ReaderCount = 0;
        }

        VOID AcquireRead() {
            ReaderLock.Acquire();
            ReaderCount++;
            if (ReaderCount == 1) {
                GlobalLock.Acquire();
            }
            ReaderLock.Release();
        }

        VOID ReleaseRead() {
            ReaderLock.Acquire();
            ReaderCount--;
            if (ReaderCount == 0) {
                GlobalLock.Release();
            }
            ReaderLock.Release();
        }

        void AcquireWrite() {
        // Writer langsung nyoba ngunci gerbang utama.
        // Kalau masih ada Reader di dalem (karena Reader pertama tadi ngunci ini), 
        // Writer bakal ketahan (block/spin) disini sampe Reader terakhir keluar.
        GlobalLock.Acquire();
        }

        // Dipanggil pas kelar NULIS
        void ReleaseWrite() {
            GlobalLock.Release();
        }
};