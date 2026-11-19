/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * OpalineOS - minimal UEFI definitions.
 *
 * Only the parts of the UEFI 2.x specification that the bootloader uses.
 * Field order and sizes follow the specification exactly; unused function
 * pointers are kept as opaque `void *` so the structure layout stays correct.
 */
#ifndef OPALINE_EFI_H
#define OPALINE_EFI_H

typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef unsigned long long UINTN;
typedef UINT16             CHAR16;
typedef UINT8              BOOLEAN;
typedef UINTN              EFI_STATUS;
typedef void              *EFI_HANDLE;
typedef void              *EFI_EVENT;

#define EFIAPI __attribute__((ms_abi))

#define EFI_SUCCESS     0ULL
#define EFI_ERROR_BIT   (1ULL << 63)
#define EFI_NOT_READY   (EFI_ERROR_BIT | 6)

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* ---- Text output ---------------------------------------------------- */

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef EFI_STATUS(EFIAPI *EFI_TEXT_STRING)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, const CHAR16 *str);
typedef EFI_STATUS(EFIAPI *EFI_TEXT_SET_ATTRIBUTE)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, UINTN attr);
typedef EFI_STATUS(EFIAPI *EFI_TEXT_CLEAR_SCREEN)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self);

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void                  *Reset;
    EFI_TEXT_STRING        OutputString;
    void                  *TestString;
    void                  *QueryMode;
    void                  *SetMode;
    EFI_TEXT_SET_ATTRIBUTE SetAttribute;
    EFI_TEXT_CLEAR_SCREEN  ClearScreen;
    void                  *SetCursorPosition;
    void                  *EnableCursor;
    void                  *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

#define EFI_LIGHTGRAY     0x07
#define EFI_WHITE         0x0F
#define EFI_LIGHTMAGENTA  0x0D
#define EFI_LIGHTCYAN     0x0B
#define EFI_BACKGROUND_BLACK 0x00

/* ---- Text input ----------------------------------------------------- */

typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
typedef EFI_STATUS(EFIAPI *EFI_INPUT_RESET)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self, BOOLEAN extended);
typedef EFI_STATUS(EFIAPI *EFI_INPUT_READ_KEY)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self, EFI_INPUT_KEY *key);

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_INPUT_RESET    Reset;
    EFI_INPUT_READ_KEY ReadKeyStroke;
    EFI_EVENT          WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

/* ---- Boot services (only the members up to Stall are typed) -------- */

typedef EFI_STATUS(EFIAPI *EFI_STALL)(UINTN microseconds);
typedef EFI_STATUS(EFIAPI *EFI_SET_WATCHDOG_TIMER)(UINTN timeout, UINT64 code, UINTN size, CHAR16 *data);

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    void *AllocatePages;
    void *FreePages;
    void *GetMemoryMap;
    void *AllocatePool;
    void *FreePool;
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    void *HandleProtocol;
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    void *ExitBootServices;
    void *GetNextMonotonicCount;
    EFI_STALL Stall;
    EFI_SET_WATCHDOG_TIMER SetWatchdogTimer;
} EFI_BOOT_SERVICES;

/* ---- Runtime services (only the members up to ResetSystem) --------- */

typedef enum {
    EfiResetCold,
    EfiResetWarm,
    EfiResetShutdown,
} EFI_RESET_TYPE;

typedef void(EFIAPI *EFI_RESET_SYSTEM)(EFI_RESET_TYPE type, EFI_STATUS status, UINTN size, void *data);

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *GetTime;
    void *SetTime;
    void *GetWakeupTime;
    void *SetWakeupTime;
    void *SetVirtualAddressMap;
    void *ConvertPointer;
    void *GetVariable;
    void *GetNextVariableName;
    void *SetVariable;
    void *GetNextHighMonotonicCount;
    EFI_RESET_SYSTEM ResetSystem;
} EFI_RUNTIME_SERVICES;

/* ---- System table --------------------------------------------------- */

typedef struct {
    EFI_TABLE_HEADER                 Hdr;
    CHAR16                          *FirmwareVendor;
    UINT32                           FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    UINTN                            NumberOfTableEntries;
    void                            *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif
