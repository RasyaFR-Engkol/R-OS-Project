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
};