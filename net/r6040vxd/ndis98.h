/*
 * ndis98.h - the subset of the NT-style NDIS 3.0 miniport interface that
 * R6040.SYS uses, as Windows 98 SE's NDIS.VXD exports it under the module
 * name "NDIS.SYS".
 *
 * Written from the public NDIS documentation and checked against the Win98
 * debug NDIS.VXD (DDK\DBG_SYM): structure sizes, the NDIS_MINIPORT_BLOCK
 * offsets used by the receive-indication macros and the string flavour
 * (UNICODE) of the configuration calls.  See README.md, "ABI notes".
 *
 * All NDIS entry points and all miniport handlers are __stdcall; the driver
 * is compiled with -ecd (stdcall default).
 *
 * MIT License - see LICENSE.
 */
#ifndef NDIS98_H
#define NDIS98_H

typedef unsigned char   UCHAR, BOOLEAN, *PUCHAR, *PBOOLEAN;
typedef unsigned short  USHORT, *PUSHORT, WCHAR;
typedef unsigned long   ULONG, *PULONG;
typedef long            LONG;
typedef unsigned int    UINT, *PUINT;
typedef void            VOID, *PVOID;
typedef ULONG           NDIS_STATUS, *PNDIS_STATUS;
typedef PVOID           NDIS_HANDLE, *PNDIS_HANDLE;
typedef ULONG           NDIS_OID;

#ifndef NULL
#define NULL ((void *)0)
#endif
#define TRUE  1
#define FALSE 0

typedef struct {
    ULONG LowPart;
    LONG  HighPart;
} NDIS_PHYSICAL_ADDRESS, *PNDIS_PHYSICAL_ADDRESS;

typedef struct {                 /* UNICODE_STRING */
    USHORT Length;               /* bytes, without terminator */
    USHORT MaximumLength;
    WCHAR *Buffer;
} NDIS_STRING, *PNDIS_STRING;

/* opaque objects owned by NDIS */
typedef struct _NDIS_PACKET NDIS_PACKET, *PNDIS_PACKET;
typedef struct _NDIS_BUFFER {    /* only Next is used: offset 0 in both the */
    struct _NDIS_BUFFER *Next;   /* Win9x NDIS_BUFFER and the NT MDL        */
} NDIS_BUFFER, *PNDIS_BUFFER;

/* caller-allocated storage for NdisMRegisterInterrupt (NT: ~0x5C bytes) */
typedef struct {
    ULONG opaque[64];
} NDIS_MINIPORT_INTERRUPT, *PNDIS_MINIPORT_INTERRUPT;

/* ---- status codes ---- */
#define NDIS_STATUS_SUCCESS             0x00000000UL
#define NDIS_STATUS_PENDING             0x00000103UL
#define NDIS_STATUS_MEDIA_CONNECT       0x4001000BUL
#define NDIS_STATUS_MEDIA_DISCONNECT    0x4001000CUL
#define NDIS_STATUS_FAILURE             0xC0000001UL
#define NDIS_STATUS_RESOURCES           0xC000009AUL
#define NDIS_STATUS_NOT_SUPPORTED       0xC00000BBUL
#define NDIS_STATUS_ADAPTER_NOT_FOUND   0xC0010006UL
#define NDIS_STATUS_MULTICAST_FULL      0xC0010009UL
#define NDIS_STATUS_INVALID_PACKET      0xC001000FUL
#define NDIS_STATUS_INVALID_LENGTH      0xC0010014UL
#define NDIS_STATUS_INVALID_DATA        0xC0010015UL
#define NDIS_STATUS_BUFFER_TOO_SHORT    0xC0010016UL
#define NDIS_STATUS_INVALID_OID         0xC0010017UL
#define NDIS_STATUS_UNSUPPORTED_MEDIA   0xC0010019UL

/* ---- enums ---- */
#define NdisMedium802_3                 0
#define NdisInterfacePci                5
#define NdisInterruptLevelSensitive     0
#define NdisParameterInteger            0
#define NdisParameterHexInteger         1
#define NdisParameterString             2
#define NdisHardwareStatusReady         0
#define NdisMediaStateConnected         0
#define NdisMediaStateDisconnected      1

/* ---- packet filter / MAC options ---- */
#define NDIS_PACKET_TYPE_DIRECTED       0x0001
#define NDIS_PACKET_TYPE_MULTICAST      0x0002
#define NDIS_PACKET_TYPE_ALL_MULTICAST  0x0004
#define NDIS_PACKET_TYPE_BROADCAST      0x0008
#define NDIS_PACKET_TYPE_PROMISCUOUS    0x0020

#define NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA 0x0001
#define NDIS_MAC_OPTION_RECEIVE_SERIALIZED  0x0002
#define NDIS_MAC_OPTION_TRANSFERS_NOT_PEND  0x0004
#define NDIS_MAC_OPTION_NO_LOOPBACK         0x0008

/* ---- OIDs ---- */
#define OID_GEN_SUPPORTED_LIST          0x00010101
#define OID_GEN_HARDWARE_STATUS         0x00010102
#define OID_GEN_MEDIA_SUPPORTED         0x00010103
#define OID_GEN_MEDIA_IN_USE            0x00010104
#define OID_GEN_MAXIMUM_LOOKAHEAD       0x00010105
#define OID_GEN_MAXIMUM_FRAME_SIZE      0x00010106
#define OID_GEN_LINK_SPEED              0x00010107
#define OID_GEN_TRANSMIT_BUFFER_SPACE   0x00010108
#define OID_GEN_RECEIVE_BUFFER_SPACE    0x00010109
#define OID_GEN_TRANSMIT_BLOCK_SIZE     0x0001010A
#define OID_GEN_RECEIVE_BLOCK_SIZE      0x0001010B
#define OID_GEN_VENDOR_ID               0x0001010C
#define OID_GEN_VENDOR_DESCRIPTION      0x0001010D
#define OID_GEN_CURRENT_PACKET_FILTER   0x0001010E
#define OID_GEN_CURRENT_LOOKAHEAD       0x0001010F
#define OID_GEN_DRIVER_VERSION          0x00010110
#define OID_GEN_MAXIMUM_TOTAL_SIZE      0x00010111
#define OID_GEN_PROTOCOL_OPTIONS        0x00010112
#define OID_GEN_MAC_OPTIONS             0x00010113
#define OID_GEN_MEDIA_CONNECT_STATUS    0x00010114
#define OID_GEN_MAXIMUM_SEND_PACKETS    0x00010115
#define OID_GEN_XMIT_OK                 0x00020101
#define OID_GEN_RCV_OK                  0x00020102
#define OID_GEN_XMIT_ERROR              0x00020103
#define OID_GEN_RCV_ERROR               0x00020104
#define OID_GEN_RCV_NO_BUFFER           0x00020105
#define OID_GEN_RCV_CRC_ERROR           0x0002020D
#define OID_802_3_PERMANENT_ADDRESS     0x01010101
#define OID_802_3_CURRENT_ADDRESS       0x01010102
#define OID_802_3_MULTICAST_LIST        0x01010103
#define OID_802_3_MAXIMUM_LIST_SIZE     0x01010104
#define OID_802_3_RCV_ERROR_ALIGNMENT   0x01020101
#define OID_802_3_XMIT_ONE_COLLISION    0x01020102
#define OID_802_3_XMIT_MORE_COLLISIONS  0x01020103
#define OID_802_3_RCV_OVERRUN           0x01020203
#define OID_802_3_XMIT_UNDERRUN         0x01020204
#define OID_802_3_XMIT_LATE_COLLISIONS  0x01020207

/* ---- configuration ---- */
typedef struct {
    int ParameterType;
    union {
        ULONG       IntegerData;
        NDIS_STRING StringData;
    } ParameterData;
} NDIS_CONFIGURATION_PARAMETER, *PNDIS_CONFIGURATION_PARAMETER;

/* ---- resources (CM_PARTIAL_RESOURCE_LIST, packed 4 like ntddk.h) ---- */
#pragma pack(push, 4)
typedef struct {
    UCHAR  Type;
    UCHAR  ShareDisposition;
    USHORT Flags;
    union {
        struct { ULONG StartLow; LONG StartHigh; ULONG Length; } Port;
        struct { ULONG Level; ULONG Vector; ULONG Affinity; } Interrupt;
        struct { ULONG StartLow; LONG StartHigh; ULONG Length; } Memory;
        struct { ULONG Data[3]; } Generic;
    } u;
} CM_PARTIAL_RESOURCE_DESCRIPTOR;

typedef struct {
    USHORT Version;
    USHORT Revision;
    ULONG  Count;
    CM_PARTIAL_RESOURCE_DESCRIPTOR PartialDescriptors[1];
} NDIS_RESOURCE_LIST, *PNDIS_RESOURCE_LIST;
#pragma pack(pop)

#define CmResourceTypePort      1
#define CmResourceTypeInterrupt 2

/* ---- miniport handlers ---- */
typedef BOOLEAN     (__stdcall *W_CHECK_FOR_HANG_HANDLER)(NDIS_HANDLE);
typedef VOID        (__stdcall *W_DISABLE_INTERRUPT_HANDLER)(NDIS_HANDLE);
typedef VOID        (__stdcall *W_ENABLE_INTERRUPT_HANDLER)(NDIS_HANDLE);
typedef VOID        (__stdcall *W_HALT_HANDLER)(NDIS_HANDLE);
typedef VOID        (__stdcall *W_HANDLE_INTERRUPT_HANDLER)(NDIS_HANDLE);
typedef NDIS_STATUS (__stdcall *W_INITIALIZE_HANDLER)(PNDIS_STATUS, PUINT,
                        UINT *, UINT, NDIS_HANDLE, NDIS_HANDLE);
typedef VOID        (__stdcall *W_ISR_HANDLER)(PBOOLEAN, PBOOLEAN, NDIS_HANDLE);
typedef NDIS_STATUS (__stdcall *W_QUERY_INFORMATION_HANDLER)(NDIS_HANDLE,
                        NDIS_OID, PVOID, ULONG, PULONG, PULONG);
typedef NDIS_STATUS (__stdcall *W_RECONFIGURE_HANDLER)(PNDIS_STATUS,
                        NDIS_HANDLE, NDIS_HANDLE);
typedef NDIS_STATUS (__stdcall *W_RESET_HANDLER)(PBOOLEAN, NDIS_HANDLE);
typedef NDIS_STATUS (__stdcall *W_SEND_HANDLER)(NDIS_HANDLE, PNDIS_PACKET, UINT);
typedef NDIS_STATUS (__stdcall *W_SET_INFORMATION_HANDLER)(NDIS_HANDLE,
                        NDIS_OID, PVOID, ULONG, PULONG, PULONG);
typedef NDIS_STATUS (__stdcall *W_TRANSFER_DATA_HANDLER)(PUINT, PNDIS_PACKET,
                        NDIS_HANDLE, NDIS_HANDLE, UINT, UINT);
typedef VOID        (__stdcall *ADAPTER_SHUTDOWN_HANDLER)(PVOID);

/* NDIS30_MINIPORT_CHARACTERISTICS: NDIS.VXD requires length >= 0x3C */
typedef struct {
    UCHAR  MajorNdisVersion;
    UCHAR  MinorNdisVersion;
    USHORT Filler;
    UINT   Reserved;
    W_CHECK_FOR_HANG_HANDLER    CheckForHangHandler;
    W_DISABLE_INTERRUPT_HANDLER DisableInterruptHandler;
    W_ENABLE_INTERRUPT_HANDLER  EnableInterruptHandler;
    W_HALT_HANDLER              HaltHandler;
    W_HANDLE_INTERRUPT_HANDLER  HandleInterruptHandler;
    W_INITIALIZE_HANDLER        InitializeHandler;
    W_ISR_HANDLER               ISRHandler;
    W_QUERY_INFORMATION_HANDLER QueryInformationHandler;
    W_RECONFIGURE_HANDLER       ReconfigureHandler;
    W_RESET_HANDLER             ResetHandler;
    W_SEND_HANDLER              SendHandler;
    W_SET_INFORMATION_HANDLER   SetInformationHandler;
    W_TRANSFER_DATA_HANDLER     TransferDataHandler;
} NDIS_MINIPORT_CHARACTERISTICS;

/* ---- receive indication: NT ndis.h macros, Win98 block offsets ---- */
typedef VOID (__stdcall *ETH_RCV_INDICATE_HANDLER)(PVOID Filter,
        NDIS_HANDLE MacReceiveContext, PUCHAR Address, PVOID HeaderBuffer,
        UINT HeaderBufferSize, PVOID LookaheadBuffer, UINT LookaheadBufferSize,
        UINT PacketSize);
typedef VOID (__stdcall *ETH_RCV_COMPLETE_HANDLER)(PVOID Filter);

#define MINIPORT_BLOCK_ETHDB            0xD8
#define MINIPORT_BLOCK_ETHRXINDICATE    0x164
#define MINIPORT_BLOCK_ETHRXCOMPLETE    0x170
#define MB_FIELD(_h, _off, _type)  (*(_type *)((PUCHAR)(_h) + (_off)))

#define NdisMEthIndicateReceive(_H, _C, _B, _SZ, _L, _LSZ, _PSZ)          \
    MB_FIELD(_H, MINIPORT_BLOCK_ETHRXINDICATE, ETH_RCV_INDICATE_HANDLER)( \
        MB_FIELD(_H, MINIPORT_BLOCK_ETHDB, PVOID),                         \
        (_C), (PUCHAR)(_B), (_B), (_SZ), (_L), (_LSZ), (_PSZ))

#define NdisMEthIndicateReceiveComplete(_H)                                \
    MB_FIELD(_H, MINIPORT_BLOCK_ETHRXCOMPLETE, ETH_RCV_COMPLETE_HANDLER)( \
        MB_FIELD(_H, MINIPORT_BLOCK_ETHDB, PVOID))

/* ---- imports from NDIS.SYS (see the IMPORT lines in the makefile) ---- */
VOID __stdcall NdisInitializeWrapper(PNDIS_HANDLE NdisWrapperHandle,
        PVOID SystemSpecific1, PVOID SystemSpecific2, PVOID SystemSpecific3);
VOID __stdcall NdisTerminateWrapper(NDIS_HANDLE NdisWrapperHandle,
        PVOID SystemSpecific);
NDIS_STATUS __stdcall NdisMRegisterMiniport(NDIS_HANDLE NdisWrapperHandle,
        NDIS_MINIPORT_CHARACTERISTICS *MiniportCharacteristics,
        UINT CharacteristicsLength);
NDIS_STATUS __stdcall NdisAllocateMemory(PVOID *VirtualAddress, UINT Length,
        UINT MemoryFlags, NDIS_PHYSICAL_ADDRESS HighestAcceptableAddress);
VOID __stdcall NdisFreeMemory(PVOID VirtualAddress, UINT Length,
        UINT MemoryFlags);
VOID __stdcall NdisOpenConfiguration(PNDIS_STATUS Status,
        PNDIS_HANDLE ConfigurationHandle, NDIS_HANDLE WrapperConfigurationContext);
VOID __stdcall NdisReadConfiguration(PNDIS_STATUS Status,
        PNDIS_CONFIGURATION_PARAMETER *ParameterValue,
        NDIS_HANDLE ConfigurationHandle, PNDIS_STRING Keyword, int ParameterType);
VOID __stdcall NdisCloseConfiguration(NDIS_HANDLE ConfigurationHandle);
VOID __stdcall NdisWriteConfiguration(PNDIS_STATUS Status,
        NDIS_HANDLE ConfigurationHandle, PNDIS_STRING Keyword,
        PNDIS_CONFIGURATION_PARAMETER ParameterValue);
VOID __stdcall NdisMSetAttributes(NDIS_HANDLE MiniportAdapterHandle,
        NDIS_HANDLE MiniportAdapterContext, BOOLEAN BusMaster, int AdapterType);
NDIS_STATUS __stdcall NdisMPciAssignResources(NDIS_HANDLE MiniportHandle,
        ULONG SlotNumber, PNDIS_RESOURCE_LIST *AssignedResources);
VOID __stdcall NdisMQueryAdapterResources(PNDIS_STATUS Status,
        NDIS_HANDLE WrapperConfigurationContext, PNDIS_RESOURCE_LIST ResourceList,
        PUINT BufferSize);
ULONG __stdcall NdisReadPciSlotInformation(NDIS_HANDLE NdisAdapterHandle,
        ULONG SlotNumber, ULONG Offset, PVOID Buffer, ULONG Length);
ULONG __stdcall NdisWritePciSlotInformation(NDIS_HANDLE NdisAdapterHandle,
        ULONG SlotNumber, ULONG Offset, PVOID Buffer, ULONG Length);
NDIS_STATUS __stdcall NdisMRegisterIoPortRange(PVOID *PortOffset,
        NDIS_HANDLE MiniportAdapterHandle, UINT InitialPort, UINT NumberOfPorts);
VOID __stdcall NdisMDeregisterIoPortRange(NDIS_HANDLE MiniportAdapterHandle,
        UINT InitialPort, UINT NumberOfPorts, PVOID PortOffset);
VOID __stdcall NdisMAllocateSharedMemory(NDIS_HANDLE MiniportAdapterHandle,
        ULONG Length, BOOLEAN Cached, PVOID *VirtualAddress,
        PNDIS_PHYSICAL_ADDRESS PhysicalAddress);
VOID __stdcall NdisMFreeSharedMemory(NDIS_HANDLE MiniportAdapterHandle,
        ULONG Length, BOOLEAN Cached, PVOID VirtualAddress,
        NDIS_PHYSICAL_ADDRESS PhysicalAddress);
NDIS_STATUS __stdcall NdisMRegisterInterrupt(PNDIS_MINIPORT_INTERRUPT Interrupt,
        NDIS_HANDLE MiniportAdapterHandle, UINT InterruptVector,
        UINT InterruptLevel, BOOLEAN RequestIsr, BOOLEAN SharedInterrupt,
        int InterruptMode);
VOID __stdcall NdisMDeregisterInterrupt(PNDIS_MINIPORT_INTERRUPT Interrupt);
VOID __stdcall NdisMRegisterAdapterShutdownHandler(NDIS_HANDLE MiniportHandle,
        PVOID ShutdownContext, ADAPTER_SHUTDOWN_HANDLER ShutdownHandler);
VOID __stdcall NdisMDeregisterAdapterShutdownHandler(NDIS_HANDLE MiniportHandle);
VOID __stdcall NdisMSendResourcesAvailable(NDIS_HANDLE MiniportAdapterHandle);
VOID __stdcall NdisMIndicateStatus(NDIS_HANDLE MiniportAdapterHandle,
        NDIS_STATUS GeneralStatus, PVOID StatusBuffer, UINT StatusBufferSize);
VOID __stdcall NdisMIndicateStatusComplete(NDIS_HANDLE MiniportAdapterHandle);
VOID __stdcall NdisGetFirstBufferFromPacket(PNDIS_PACKET Packet,
        PNDIS_BUFFER *FirstBuffer, PVOID *FirstBufferVA,
        PUINT FirstBufferLength, PUINT TotalBufferLength);
VOID __stdcall NdisQueryBuffer(PNDIS_BUFFER Buffer, PVOID *VirtualAddress,
        PUINT Length);

#endif
