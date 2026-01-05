#pragma once

#include "../filesys/iblockdevice.hpp"
#include "../filesys/vfs/vfs.hpp"
#include "string.hpp"
#include <logging.hpp>

class ICharDevice;

#define MAX_BLOCK_DEVICE 16
#define MAX_CHAR_DEVICE 16

typedef U64 REQUEST;
typedef ANSI_STRING DEVICE_NAME;

namespace DeviceManager{
    extern IBlockDevice *g_BlockDevices[MAX_BLOCK_DEVICE];
    extern U32 g_BlockDeviceCount;

    extern ICharDevice *g_CharDevices[MAX_CHAR_DEVICE];
    extern U32 g_CharDeviceCount;

    BOOL RegisterBlockDevice(IBlockDevice *Device);

    BOOL UnregisterBlockDevice(IBlockDevice *Device);

    BOOL RegisterCharDevice(ICharDevice *Device);

    U32 GetBlockDeviceCount();
    IBlockDevice *GetBlockDevice(U32 Index);

    IBlockDevice* FindBlockDevice(const char* name);
    ICharDevice* FindCharDevice(const char* name);

    namespace StorageManager{
        VOID SyncAllStorageDevices();
    }
}

namespace DeviceManager{
    #define MAX_REQUEST_SLOT 32
    typedef VOID* (*HandleFunction)(POINTER Anything);
    struct _DispatchTable{
        REQUEST RequestID;
        HandleFunction Function;
    };
    enum ObjectType{
        OT_CHAR,
        OT_BLOCK,
        OT_FILE,
        OT_DISPLAY,
        OT_NET,
        OT_OTHER
    };
    struct DeviceObject{
        CHAR8 NameDevice[64];
        ObjectType Type;
        U64 DeviceID;
        Arch::Spinlock::Spinlock Lock;
        VOID *DriverInstance;
        DeviceObject *Next;
        _DispatchTable FunctionTableing[MAX_REQUEST_SLOT];
        U64 FunctionTableCount;
    };
    class DevOBJManaager{
        private:
            BOOL IsOBJInitialized;
            DeviceObject *Head;
            U64 NextIDCounter;
        public:
            DevOBJManaager(){
                FirstInitializeDevOBJManager();
            }

            ~DevOBJManaager(){}
            BOOLFUNC FirstInitializeDevOBJManager(){
                IsOBJInitialized = TRUE;
                Head = nullptr;
                NextIDCounter = 1;
                return TRUE;
            }

            BOOLFUNC CheckIfExist(CONSTANT ANSI_STRING *Name){
                DeviceObject *current = Head;
                while(current != nullptr){
                    if(String::Strcmp(current->NameDevice, Name) == 0) return TRUE;
                    current = current->Next;
                }
                return FALSE;
            }

            DEVICEHANDLE GiveInstance(CONSTANT ANSI_STRING *Name, DeviceObject **ObjOut){
                DeviceObject *current = Head;
                while(current != nullptr){
                    if(String::Strcmp(current->NameDevice, Name) == 0){
                        if(ObjOut) *ObjOut = current;
                        return current;
                    }
                    current = current->Next;
                }
                *ObjOut = nullptr;
                return nullptr;
            }

            RHANDLE RegisterDeviceInstance(const char *name, ObjectType Type, VOID *DriverPTR){
                if(CheckIfExist(name)) {
                    DeviceObject *Handle = nullptr;
                    if(GiveInstance(name, &Handle)){
                        return Handle;
                    }
                }
                DeviceObject *NewNode = new DeviceObject();
                if(!NewNode) return 0;

                String::Memset(NewNode, 0, sizeof(DeviceObject));
                String::Strcpy(NewNode->NameDevice, name);
                NewNode->Type = Type;
                NewNode->DriverInstance = DriverPTR;
                NewNode->DeviceID = NextIDCounter++;
                NewNode->Lock.Init();

                NewNode->Next = Head;
                Head = NewNode;

                for(INTN i = 0; i < MAX_REQUEST_SLOT; i++){
                    NewNode->FunctionTableing[i].Function = nullptr;
                    NewNode->FunctionTableing[i].RequestID = 0;
                }

                return NewNode;
            }

            REQUEST RegisterFunctionToTable(CONSTANT DEVICE_NAME *NameDevice, REQUEST RequestID, HandleFunction FunctionHandle){
                DeviceObject *current = Head;
                while(current != nullptr){
                    if(String::Strcmp(current->NameDevice, NameDevice) == 0){
                        current->FunctionTableing[current->FunctionTableCount].RequestID = RequestID;
                        current->FunctionTableing[current->FunctionTableCount].Function = FunctionHandle;
                        current->FunctionTableCount++;
                        return RequestID;
                    }
                    current = current->Next;
                }
                return 0;
            }

            RHANDLE RequestStructOnDevice(DeviceObject* DevHandle, REQUEST ID, POINTER Struct){
                // 1. Validasi Handle
                if (!DevHandle) return nullptr;

                // 2. Cari Fungsi di Tabel Dispatch Driver
                // Kita harus Loop karena kamu pakai sistem Append (FunctionTableCount)
                for(U64 i = 0; i < DevHandle->FunctionTableCount; i++){
                    
                    // Cek apakah ID di slot ini cocok dengan yang diminta user?
                    if(DevHandle->FunctionTableing[i].RequestID == ID){
                        
                        // 3. Ambil Pointer Fungsinya
                        HandleFunction TargetFunc = DevHandle->FunctionTableing[i].Function;
                        
                        // 4. EKSEKUSI!
                        // Di sini magic-nya. Kita panggil fungsi driver, lempar struct-nya,
                        // dan tangkap nilai kembaliannya (RHANDLE/Memory Address).
                        if(TargetFunc) {
                            return TargetFunc(Struct); 
                        }
                    }
                }

                // Kalau loop selesai dan gak ketemu ID-nya
                return nullptr; 
            }
    };
    extern DevOBJManaager ObjectManager;
}