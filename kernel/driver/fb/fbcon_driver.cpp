#include "fbcon_driver.hpp"
#include <string.hpp>
#include <logging.hpp>
#include <rosval.h>
#include "../../log/fbcon/fbcon.hpp"

/* Minimal tty-like filesystem front-end that writes to framebuffer console.
 * This implements a tiny FileSystem-backed device where writes are mirrored
 * to the FB via FBConsole::WriteString(). Read is not implemented (returns 0).
 */

ttyFB0::ttyFB0(){}

ttyFB0::~ttyFB0(){}

U32 ttyFB0::Read(File* file, U8* buffer, U32 size){
    // No readable data from the framebuffer console device
    (void)file; (void)buffer; (void)size;
    return 0;
}

U32 ttyFB0::Write(File *file, U8 *buffer, U32 size){
    if(!file || !buffer || size == 0) return 0;

    // Allocate a temporary null-terminated buffer for FBConsole::WriteString
    CHAR8 *tmp = new CHAR8[size + 1];
    if(!tmp) return 0;
    for(U32 i = 0; i < size; ++i) tmp[i] = (CHAR8)buffer[i];
    tmp[size] = 0;

    // Mirror to framebuffer console
    if(FBConsole::IsReady()){
        FBConsole::WriteString(tmp);
    }

    delete[] tmp;
    // Report all bytes consumed
    return size;
}

void ttyFB0::Close(File* file){
    if(!file) return;
    delete file;
}


