// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once
// ============================================================
// Starlix OS - Minimal Freestanding UEFI API
// OpenRPX xsOft - Built from scratch (zero STL dependency)
// ============================================================

// ------------------- Freestanding fundamental types -------------------
// NOTE: We DO NOT include <cstdint>/<cstddef> because the x86_64
// UEFI target has no hosted C++ standard library. These are
// hand-rolled primitive types for a pure freestanding environment.

using int8_t   = signed char;
using uint8_t  = unsigned char;
using int16_t  = signed short;
using uint16_t = unsigned short;
using int32_t  = signed int;
using uint32_t = unsigned int;
using int64_t  = signed long long;
using uint64_t = unsigned long long;
using size_t   = uint64_t;
using uintptr_t = uint64_t;

// char16_t is a built-in keyword in C++11+, no header needed.

extern "C" {

using EFI_STATUS = uint64_t;
using EFI_HANDLE = void*;
using EFI_EVENT  = void*;
using CHAR16     = char16_t;
using BOOLEAN    = unsigned char;
using UINTN      = uint64_t;
using UINT32     = uint32_t;
using UINT64     = uint64_t;

#define TRUE  1
#define FALSE 0

#define EFI_SUCCESS            0ULL
#define EFI_ERR(x)             (0x8000000000000000ULL | (x))
#define EFI_LOAD_ERROR         EFI_ERR(1)
#define EFI_INVALID_PARAMETER  EFI_ERR(2)
#define EFI_BUFFER_TOO_SMALL   EFI_ERR(5)
#define EFI_NOT_FOUND          EFI_ERR(14)

#define EFI_FILE_MODE_READ     0x0000000000000001ULL

#define ABI __attribute__((ms_abi))

struct EFI_GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
};

struct EFI_TABLE_HEADER {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
};

// ------------------- Simple Text Output -------------------
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (ABI *EFI_TEXT_RESET)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, BOOLEAN);
typedef EFI_STATUS (ABI *EFI_TEXT_STRING)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, CHAR16*);
typedef EFI_STATUS (ABI *EFI_TEXT_TEST_STRING)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, CHAR16*);
typedef EFI_STATUS (ABI *EFI_TEXT_QUERY_MODE)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, UINTN, UINTN*, UINTN*);
typedef EFI_STATUS (ABI *EFI_TEXT_SET_MODE)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, UINTN);
typedef EFI_STATUS (ABI *EFI_TEXT_SET_ATTRIBUTE)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, UINTN);
typedef EFI_STATUS (ABI *EFI_TEXT_CLEAR_SCREEN)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*);
typedef EFI_STATUS (ABI *EFI_TEXT_SET_CURSOR_POSITION)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, UINTN, UINTN);
typedef EFI_STATUS (ABI *EFI_TEXT_ENABLE_CURSOR)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, BOOLEAN);

struct SIMPLE_TEXT_OUTPUT_MODE {
    int32_t MaxMode;
    int32_t Mode;
    int32_t Attribute;
    int32_t CursorColumn;
    int32_t CursorRow;
    BOOLEAN CursorVisible;
};

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET               Reset;
    EFI_TEXT_STRING              OutputString;
    EFI_TEXT_TEST_STRING         TestString;
    EFI_TEXT_QUERY_MODE          QueryMode;
    EFI_TEXT_SET_MODE            SetMode;
    EFI_TEXT_SET_ATTRIBUTE       SetAttribute;
    EFI_TEXT_CLEAR_SCREEN        ClearScreen;
    EFI_TEXT_SET_CURSOR_POSITION SetCursorPosition;
    EFI_TEXT_ENABLE_CURSOR       EnableCursor;
    SIMPLE_TEXT_OUTPUT_MODE*     Mode;
};

// ------------------- Boot Services -------------------
struct EFI_BOOT_SERVICES;
struct EFI_RUNTIME_SERVICES;
struct EFI_SYSTEM_TABLE;

typedef EFI_STATUS (ABI *EFI_ALLOCATE_POOL)(UINTN, UINTN, void**);
typedef EFI_STATUS (ABI *EFI_FREE_POOL)(void*);
typedef EFI_STATUS (ABI *EFI_HANDLE_PROTOCOL)(EFI_HANDLE, EFI_GUID*, void**);
typedef EFI_STATUS (ABI *EFI_LOCATE_PROTOCOL)(EFI_GUID*, void*, void**);
typedef EFI_STATUS (ABI *EFI_LOAD_IMAGE)(BOOLEAN, EFI_HANDLE, void*, void*, UINTN, EFI_HANDLE*);
typedef EFI_STATUS (ABI *EFI_START_IMAGE)(EFI_HANDLE, UINTN*, CHAR16**);
typedef EFI_STATUS (ABI *EFI_EXIT)(EFI_HANDLE, EFI_STATUS, UINTN, CHAR16*);
typedef EFI_STATUS (ABI *EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE, UINTN);
typedef EFI_STATUS (ABI *EFI_OPEN_PROTOCOL)(EFI_HANDLE, EFI_GUID*, void**, EFI_HANDLE, EFI_HANDLE, UINT32);
typedef EFI_STATUS (ABI *EFI_CLOSE_PROTOCOL)(EFI_HANDLE, EFI_GUID*, EFI_HANDLE, EFI_HANDLE);
typedef EFI_STATUS (ABI *EFI_LOCATE_HANDLE_BUFFER)(uint32_t, EFI_GUID*, void*, UINTN*, EFI_HANDLE**);

struct EFI_MEMORY_DESCRIPTOR {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
};

typedef EFI_STATUS (ABI *EFI_GET_MEMORY_MAP)(
    UINTN*, EFI_MEMORY_DESCRIPTOR*, UINTN*, UINTN*, UINT32*);

typedef EFI_STATUS (ABI *EFI_ALLOCATE_PAGES)(
    UINTN, UINTN, UINTN, uint64_t*);

typedef EFI_STATUS (ABI *EFI_STALL)(UINTN);


#define AllocateAnyPages 0
#define AllocateAddress 2
#define EfiLoaderData 2


struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;

    void* RaiseTPL;
    void* RestoreTPL;

    EFI_ALLOCATE_PAGES AllocatePages;
    void* FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    EFI_ALLOCATE_POOL AllocatePool;
    EFI_FREE_POOL     FreePool;

    void* CreateEvent;
    void* SetTimer;
    void* WaitForEvent;
    void* SignalEvent;
    void* CloseEvent;
    void* CheckEvent;

    void* InstallProtocolInterface;
    void* ReinstallProtocolInterface;
    void* UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL HandleProtocol;
    void* Reserved;
    void* RegisterProtocolNotify;
    void* LocateHandle;
    void* LocateDevicePath;
    void* InstallConfigurationTable;

    EFI_LOAD_IMAGE  LoadImage;
    EFI_START_IMAGE StartImage;
    EFI_EXIT        Exit;
    void* UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;

    void* GetNextMonotonicCount;
    EFI_STALL Stall;
    void* SetWatchdogTimer;

    void* ConnectController;
    void* DisconnectController;

    EFI_OPEN_PROTOCOL  OpenProtocol;
    EFI_CLOSE_PROTOCOL CloseProtocol;
    void* OpenProtocolInformation;

    void* ProtocolsPerHandle;
    EFI_LOCATE_HANDLE_BUFFER LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL LocateProtocol;
    void* InstallMultipleProtocolInterfaces;
    void* UninstallMultipleProtocolInterfaces;

    void* CalculateCrc32;

    void* CopyMem;
    void* SetMem;
    void* CreateEventEx;
};

struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    CHAR16* FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void* ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* StdErr;
    EFI_RUNTIME_SERVICES* RuntimeServices;
    EFI_BOOT_SERVICES* BootServices;
    UINTN NumberOfTableEntries;
    void* ConfigurationTable;
};

// ------------------- Loaded Image Protocol -------------------
struct EFI_LOADED_IMAGE_PROTOCOL {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE* SystemTable;
    EFI_HANDLE DeviceHandle;
    void* FilePath;
    void* Reserved;
    uint32_t LoadOptionsSize;
    void* LoadOptions;
    void* ImageBase;
    uint64_t ImageSize;
    uint32_t ImageCodeType;
    uint32_t ImageDataType;
    void* Unload;
};

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11d2, {0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B} }

// ------------------- File System Protocol -------------------
struct EFI_FILE_PROTOCOL;

typedef EFI_STATUS (ABI *EFI_FILE_OPEN)(EFI_FILE_PROTOCOL*, EFI_FILE_PROTOCOL**, CHAR16*, uint64_t, uint64_t);
typedef EFI_STATUS (ABI *EFI_FILE_CLOSE)(EFI_FILE_PROTOCOL*);
typedef EFI_STATUS (ABI *EFI_FILE_READ)(EFI_FILE_PROTOCOL*, UINTN*, void*);
typedef EFI_STATUS (ABI *EFI_FILE_GET_INFO)(EFI_FILE_PROTOCOL*, EFI_GUID*, UINTN*, void*);
typedef EFI_STATUS (ABI *EFI_FILE_SET_POSITION)(EFI_FILE_PROTOCOL*, uint64_t);

struct EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_FILE_OPEN Open;
    EFI_FILE_CLOSE Close;
    void* Delete;
    EFI_FILE_READ Read;
    void* Write;
    void* GetPosition;
    EFI_FILE_SET_POSITION SetPosition;
    EFI_FILE_GET_INFO GetInfo;
    void* SetInfo;
    void* Flush;
    void* OpenEx;
    void* ReadEx;
    void* WriteEx;
    void* FlushEx;
};

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
typedef EFI_STATUS (ABI *EFI_SFS_OPEN_VOLUME)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*, EFI_FILE_PROTOCOL**);

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t Revision;
    EFI_SFS_OPEN_VOLUME OpenVolume;
};

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, {0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b} }

} // extern "C"
