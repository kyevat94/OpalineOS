/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * OpalineOS - minimal UEFI definitions.
 *
 * Only the parts of the UEFI 2.x specification that the bootloader uses.
 * Field order and sizes follow the specification exactly; members we do not
 * call are kept as opaque `void *` so the structure layout stays correct.
 */
#ifndef OPALINE_EFI_H
#define OPALINE_EFI_H

#include <stdint.h>

typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef uint64_t  UINTN;
typedef uint16_t  CHAR16;
typedef uint8_t   BOOLEAN;
typedef UINTN     EFI_STATUS;
typedef void     *EFI_HANDLE;
typedef void     *EFI_EVENT;
typedef UINT64    EFI_PHYSICAL_ADDRESS;

#define EFIAPI __attribute__((ms_abi))

#define EFI_SUCCESS            0ULL
#define EFI_ERROR_BIT          (1ULL << 63)
#define EFI_ERROR(s)           (((s) & EFI_ERROR_BIT) != 0)
#define EFI_INVALID_PARAMETER  (EFI_ERROR_BIT | 2)
#define EFI_BUFFER_TOO_SMALL   (EFI_ERROR_BIT | 5)
#define EFI_NOT_READY          (EFI_ERROR_BIT | 6)
#define EFI_NOT_FOUND          (EFI_ERROR_BIT | 14)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* ---- Memory --------------------------------------------------------- */

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiUnacceptedMemoryType,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
} EFI_ALLOCATE_TYPE;

typedef struct {
    UINT32               Type;
    UINT32               Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    UINT64               VirtualStart;
    UINT64               NumberOfPages;
    UINT64               Attribute;
} EFI_MEMORY_DESCRIPTOR;

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

#define EFI_LIGHTGRAY         0x07
#define EFI_LIGHTRED          0x0C
#define EFI_LIGHTMAGENTA      0x0D
#define EFI_BACKGROUND_BLACK  0x00

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

/* ---- Boot services -------------------------------------------------- */

typedef EFI_STATUS(EFIAPI *EFI_ALLOCATE_PAGES)(EFI_ALLOCATE_TYPE type, UINT32 memory_type, UINTN pages,
                                               EFI_PHYSICAL_ADDRESS *memory);
typedef EFI_STATUS(EFIAPI *EFI_GET_MEMORY_MAP)(UINTN *map_size, EFI_MEMORY_DESCRIPTOR *map, UINTN *map_key,
                                               UINTN *descriptor_size, UINT32 *descriptor_version);
typedef EFI_STATUS(EFIAPI *EFI_ALLOCATE_POOL)(UINT32 pool_type, UINTN size, void **buffer);
typedef EFI_STATUS(EFIAPI *EFI_FREE_POOL)(void *buffer);
typedef EFI_STATUS(EFIAPI *EFI_HANDLE_PROTOCOL)(EFI_HANDLE handle, const EFI_GUID *protocol, void **interface);
typedef EFI_STATUS(EFIAPI *EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE image, UINTN map_key);
typedef EFI_STATUS(EFIAPI *EFI_STALL)(UINTN microseconds);
typedef EFI_STATUS(EFIAPI *EFI_SET_WATCHDOG_TIMER)(UINTN timeout, UINT64 code, UINTN size, CHAR16 *data);
typedef EFI_STATUS(EFIAPI *EFI_LOCATE_PROTOCOL)(const EFI_GUID *protocol, void *registration, void **interface);

typedef struct {
    EFI_TABLE_HEADER       Hdr;
    void                  *RaiseTPL;
    void                  *RestoreTPL;
    EFI_ALLOCATE_PAGES     AllocatePages;
    void                  *FreePages;
    EFI_GET_MEMORY_MAP     GetMemoryMap;
    EFI_ALLOCATE_POOL      AllocatePool;
    EFI_FREE_POOL          FreePool;
    void                  *CreateEvent;
    void                  *SetTimer;
    void                  *WaitForEvent;
    void                  *SignalEvent;
    void                  *CloseEvent;
    void                  *CheckEvent;
    void                  *InstallProtocolInterface;
    void                  *ReinstallProtocolInterface;
    void                  *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL    HandleProtocol;
    void                  *Reserved;
    void                  *RegisterProtocolNotify;
    void                  *LocateHandle;
    void                  *LocateDevicePath;
    void                  *InstallConfigurationTable;
    void                  *LoadImage;
    void                  *StartImage;
    void                  *Exit;
    void                  *UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;
    void                  *GetNextMonotonicCount;
    EFI_STALL              Stall;
    EFI_SET_WATCHDOG_TIMER SetWatchdogTimer;
    void                  *ConnectController;
    void                  *DisconnectController;
    void                  *OpenProtocol;
    void                  *CloseProtocol;
    void                  *OpenProtocolInformation;
    void                  *ProtocolsPerHandle;
    void                  *LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL    LocateProtocol;
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
    EFI_GUID VendorGuid;
    void    *VendorTable;
} EFI_CONFIGURATION_TABLE;

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
    EFI_CONFIGURATION_TABLE         *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---- Loaded image --------------------------------------------------- */

typedef struct {
    UINT32            Revision;
    EFI_HANDLE        ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE        DeviceHandle;
    void             *FilePath;
    void             *Reserved;
    UINT32            LoadOptionsSize;
    void             *LoadOptions;
    void             *ImageBase;
    UINT64            ImageSize;
    UINT32            ImageCodeType;
    UINT32            ImageDataType;
    void             *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ---- File system ---------------------------------------------------- */

#define EFI_FILE_MODE_READ 0x0000000000000001ULL

struct EFI_FILE_PROTOCOL;
typedef EFI_STATUS(EFIAPI *EFI_FILE_OPEN)(struct EFI_FILE_PROTOCOL *self, struct EFI_FILE_PROTOCOL **out,
                                          const CHAR16 *name, UINT64 mode, UINT64 attributes);
typedef EFI_STATUS(EFIAPI *EFI_FILE_CLOSE)(struct EFI_FILE_PROTOCOL *self);
typedef EFI_STATUS(EFIAPI *EFI_FILE_READ)(struct EFI_FILE_PROTOCOL *self, UINTN *size, void *buffer);
typedef EFI_STATUS(EFIAPI *EFI_FILE_GET_POSITION)(struct EFI_FILE_PROTOCOL *self, UINT64 *position);
typedef EFI_STATUS(EFIAPI *EFI_FILE_SET_POSITION)(struct EFI_FILE_PROTOCOL *self, UINT64 position);

typedef struct EFI_FILE_PROTOCOL {
    UINT64                Revision;
    EFI_FILE_OPEN         Open;
    EFI_FILE_CLOSE        Close;
    void                 *Delete;
    EFI_FILE_READ         Read;
    void                 *Write;
    EFI_FILE_GET_POSITION GetPosition;
    EFI_FILE_SET_POSITION SetPosition;
    void                 *GetInfo;
    void                 *SetInfo;
    void                 *Flush;
} EFI_FILE_PROTOCOL;

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
typedef EFI_STATUS(EFIAPI *EFI_OPEN_VOLUME)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self, EFI_FILE_PROTOCOL **root);

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64          Revision;
    EFI_OPEN_VOLUME OpenVolume;
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* ---- Graphics output ------------------------------------------------ */

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32                    Version;
    UINT32                    HorizontalResolution;
    UINT32                    VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK         PixelInformation;
    UINT32                    PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32                                MaxMode;
    UINT32                                Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                                 SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                  FrameBufferBase;
    UINTN                                 FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

struct EFI_GRAPHICS_OUTPUT_PROTOCOL;
typedef EFI_STATUS(EFIAPI *EFI_GOP_QUERY_MODE)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *self, UINT32 mode,
                                               UINTN *size, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info);
typedef EFI_STATUS(EFIAPI *EFI_GOP_SET_MODE)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *self, UINT32 mode);

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GOP_QUERY_MODE                 QueryMode;
    EFI_GOP_SET_MODE                   SetMode;
    void                              *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ---- GUIDs ---------------------------------------------------------- */

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11D2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042A9DE, 0x23DC, 0x4A38, { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A } }
#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868E871, 0xE4F1, 0x11D3, { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 } }
#define EFI_ACPI_10_TABLE_GUID \
    { 0xEB9D2D30, 0x2D88, 0x11D3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } }

#endif
