#include "rbt.hpp"

namespace Tasking{
    VOID RBT_LeftRotate(Task **Root, Task *X){
        Task *y = X->RbtRight;
        X->RbtRight = y->RbtLeft;

        if(y->RbtLeft != nullptr){
            y->RbtLeft->RbtParent = X;
        }

        y->RbtParent = X->RbtParent;
        
        if(X->RbtParent == nullptr){
            *Root = y;
        } else if(X == X->RbtParent->RbtLeft){
            X->RbtParent->RbtLeft = y;
        } else {
            X->RbtParent->RbtRight = y;
        }

        y->RbtLeft = X;
        X->RbtParent = y;
    }

    VOID RBT_RightRotate(Task **Root, Task *y) {
        Task *x = y->RbtLeft; // x di set ke anak kiri y
        y->RbtLeft = x->RbtRight; // Pindahkan anak kanan x jadi anak kiri y
        
        if (x->RbtRight != nullptr) {
            x->RbtRight->RbtParent = y;
        }
        
        x->RbtParent = y->RbtParent; // Sambungkan parent x ke parent y
        
        if (y->RbtParent == nullptr) {
            *Root = x; // Jika y root, sekarang x jadi root
        } else if (y == y->RbtParent->RbtRight) {
            y->RbtParent->RbtRight = x;
        } else {
            y->RbtParent->RbtLeft = x;
        }
        
        x->RbtRight = y; // Taruh y di kanan x
        y->RbtParent = x;
    }

    VOID RBT_InsertFixup(Task **Root, Task *z) {
        // Selama parent dari z adalah MERAH (melanggar aturan RBT)
        while (z->RbtParent != nullptr && z->RbtParent->Color == RBT_RED) {
            
            // Jika parent z adalah anak kiri dari kakek z
            if (z->RbtParent == z->RbtParent->RbtParent->RbtLeft) {
                Task *y = z->RbtParent->RbtParent->RbtRight; // Paman z
                
                // Kasus 1: Paman z berwarna MERAH
                if (y != nullptr && y->Color == RBT_RED) {
                    z->RbtParent->Color = RBT_BLACK;
                    y->Color = RBT_BLACK;
                    z->RbtParent->RbtParent->Color = RBT_RED;
                    z = z->RbtParent->RbtParent; // Cek kakeknya sekarang
                } 
                else {
                    // Kasus 2: z adalah anak kanan (Butuh rotasi kiri dulu)
                    if (z == z->RbtParent->RbtRight) {
                        z = z->RbtParent;
                        RBT_LeftRotate(Root, z);
                    }
                    // Kasus 3: z adalah anak kiri (Ganti warna dan rotasi kanan)
                    z->RbtParent->Color = RBT_BLACK;
                    z->RbtParent->RbtParent->Color = RBT_RED;
                    RBT_RightRotate(Root, z->RbtParent->RbtParent);
                }
            } 
            // Kebalikan dari di atas (parent z adalah anak kanan kakek z)
            else {
                Task *y = z->RbtParent->RbtParent->RbtLeft; // Paman z
                
                // Kasus 1: Paman z berwarna MERAH
                if (y != nullptr && y->Color == RBT_RED) {
                    z->RbtParent->Color = RBT_BLACK;
                    y->Color = RBT_BLACK;
                    z->RbtParent->RbtParent->Color = RBT_RED;
                    z = z->RbtParent->RbtParent;
                } 
                else {
                    // Kasus 2: z adalah anak kiri (Butuh rotasi kanan dulu)
                    if (z == z->RbtParent->RbtLeft) {
                        z = z->RbtParent;
                        RBT_RightRotate(Root, z);
                    }
                    // Kasus 3: z adalah anak kanan (Ganti warna dan rotasi kiri)
                    z->RbtParent->Color = RBT_BLACK;
                    z->RbtParent->RbtParent->Color = RBT_RED;
                    RBT_LeftRotate(Root, z->RbtParent->RbtParent);
                }
            }
        }
        // Aturan mutlak RBT: Root harus selalu HITAM
        (*Root)->Color = RBT_BLACK;
    }

    VOID RBT_Transplant(Task **Root, Task *u, Task *v) {
        if (u->RbtParent == nullptr) {
            *Root = v;
        } else if (u == u->RbtParent->RbtLeft) {
            u->RbtParent->RbtLeft = v;
        } else {
            u->RbtParent->RbtRight = v;
        }
        if (v != nullptr) {
            v->RbtParent = u->RbtParent;
        }
    }

    // Fixup setelah Delete (menjaga aturan warna hitam)
    VOID RBT_EraseFixup(Task **Root, Task *x, Task *xParent) {
        while (x != *Root && (x == nullptr || x->Color == RBT_BLACK)) {
            if (x == xParent->RbtLeft) {
                Task *w = xParent->RbtRight; // Saudara x
                if (w->Color == RBT_RED) {
                    w->Color = RBT_BLACK;
                    xParent->Color = RBT_RED;
                    RBT_LeftRotate(Root, xParent);
                    w = xParent->RbtRight;
                }
                if ((w->RbtLeft == nullptr || w->RbtLeft->Color == RBT_BLACK) &&
                    (w->RbtRight == nullptr || w->RbtRight->Color == RBT_BLACK)) {
                    w->Color = RBT_RED;
                    x = xParent;
                    xParent = x->RbtParent;
                } else {
                    if (w->RbtRight == nullptr || w->RbtRight->Color == RBT_BLACK) {
                        if (w->RbtLeft != nullptr) w->RbtLeft->Color = RBT_BLACK;
                        w->Color = RBT_RED;
                        RBT_RightRotate(Root, w);
                        w = xParent->RbtRight;
                    }
                    w->Color = xParent->Color;
                    xParent->Color = RBT_BLACK;
                    if (w->RbtRight != nullptr) w->RbtRight->Color = RBT_BLACK;
                    RBT_LeftRotate(Root, xParent);
                    x = *Root;
                }
            } else {
                Task *w = xParent->RbtLeft;
                if (w->Color == RBT_RED) {
                    w->Color = RBT_BLACK;
                    xParent->Color = RBT_RED;
                    RBT_RightRotate(Root, xParent);
                    w = xParent->RbtLeft;
                }
                if ((w->RbtRight == nullptr || w->RbtRight->Color == RBT_BLACK) &&
                    (w->RbtLeft == nullptr || w->RbtLeft->Color == RBT_BLACK)) {
                    w->Color = RBT_RED;
                    x = xParent;
                    xParent = x->RbtParent;
                } else {
                    if (w->RbtLeft == nullptr || w->RbtLeft->Color == RBT_BLACK) {
                        if (w->RbtRight != nullptr) w->RbtRight->Color = RBT_BLACK;
                        w->Color = RBT_RED;
                        RBT_LeftRotate(Root, w);
                        w = xParent->RbtLeft;
                    }
                    w->Color = xParent->Color;
                    xParent->Color = RBT_BLACK;
                    if (w->RbtLeft != nullptr) w->RbtLeft->Color = RBT_BLACK;
                    RBT_RightRotate(Root, xParent);
                    x = *Root;
                }
            }
        }
        if (x != nullptr) {
            x->Color = RBT_BLACK;
        }
    }

    // Fungsi utama untuk menghapus node z dari RBT
    VOID RBT_Erase(Task **Root, Task *z) {
        Task *y = z;
        Task *x = nullptr;
        Task *xParent = nullptr;
        RBTColor yOriginalColor = y->Color;

        if (z->RbtLeft == nullptr) {
            x = z->RbtRight;
            xParent = z->RbtParent; // Simpan parent x sebelum transplant
            RBT_Transplant(Root, z, z->RbtRight);
        } else if (z->RbtRight == nullptr) {
            x = z->RbtLeft;
            xParent = z->RbtParent;
            RBT_Transplant(Root, z, z->RbtLeft);
        } else {
            // Cari successor (node paling kiri di subtree kanan z)
            y = z->RbtRight;
            while (y->RbtLeft != nullptr) {
                y = y->RbtLeft;
            }
            yOriginalColor = y->Color;
            x = y->RbtRight;
            
            if (y->RbtParent == z) {
                xParent = y; // Jika y langsung di bawah z
            } else {
                xParent = y->RbtParent;
                RBT_Transplant(Root, y, y->RbtRight);
                y->RbtRight = z->RbtRight;
                y->RbtRight->RbtParent = y;
            }
            RBT_Transplant(Root, z, y);
            y->RbtLeft = z->RbtLeft;
            y->RbtLeft->RbtParent = y;
            y->Color = z->Color;
        }

        if (yOriginalColor == RBT_BLACK) {
            RBT_EraseFixup(Root, x, xParent);
        }

        // Bersihkan pointer node yang dihapus biar aman
        z->RbtLeft = nullptr;
        z->RbtRight = nullptr;
        z->RbtParent = nullptr;
    }
}