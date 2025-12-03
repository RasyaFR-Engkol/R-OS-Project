#pragma once

#include "rosval.h"
enum class InputEventType {
    KEYBOARD_PRESS,
    KEYBOARD_RELEASE,
    MOUSE_MOVE,
    MOUSE_CLICK,
    MOUSE_RELEASE
};

struct InputEvent{
    InputEventType Type;
    union{
        struct{
            U32 Scancode;
        } Keycode;
        struct {
            I32 dX;
            I32 dY;
            U8 Button; // 1=left, 2=right, 3=middle
        } Mouse;
    };
};