#pragma once
#include "task.hpp"

namespace Tasking{
    VOID RBT_LeftRotate(Task **Root, Task *X);
    VOID RBT_RightRotate(Task **Root, Task *Y);
    VOID RBT_InsertFixup(Task **Root, Task *Z);
    VOID RBT_Erase(Task **Root, Task *z);
}