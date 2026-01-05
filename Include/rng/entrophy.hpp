#pragma once
#include <rosval.h>

class EntrophySystem{
    private:
        static U32 m_Pool;

    public:
        static VOID AddEntrophy(const U32 Data){
            m_Pool = (m_Pool << 5) | (m_Pool >> (32 - 5)); // rotate left 5
            m_Pool ^= Data;
        }

        static U32 GetSeed(){
            return m_Pool;
        }

        // Simple Xorshift RNG based on entropy pool
        static U32 Random() {
            U32 x = m_Pool;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            m_Pool = x; // Update pool biar next random beda
            return x;
        }
};