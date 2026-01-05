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

File* ttyFB0::Open(const char* path, U32 Flags){
    File* f = new File();
    // Initialize fields explicitly (avoid memset that would clobber vptr)
    f->FileSize = 0;
    f->CurrentPosition = 0;
    f->IsDirectory = FALSE;
    f->Internal_StartCluster = 0;
    f->Internal_CurrentCluster = 0;
    f->Internal_DirEntryLBA = 0;
    f->Internal_DirEntryOffset = 0;
    f->FSOwner = this;
    // copy name (truncate if needed)
    if(path){
        unsigned long long len = String::Strlen(path);
        unsigned long long tocpy = (len < sizeof(f->FileName)-1) ? len : (sizeof(f->FileName)-1);
        String::Memcpy(f->FileName, path, tocpy);
        f->FileName[tocpy] = '\0';
    } else {
        f->FileName[0] = '\0';
    }
    return f;
}

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


