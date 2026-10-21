# makefile for R6040.SYS (Open Watcom 2.0, wmake) and R6040DBG.EXE
#   set WATCOM=C:\WATCOM
#   set INCLUDE=C:\WATCOM\h;C:\WATCOM\h\nt
#   set PATH=C:\WATCOM\binnt64;C:\WATCOM\binnt;%PATH%
#   wmake -h -e
#
# The driver is an NT-style PE kernel module (subsystem native, image base
# 10000h, 512-byte section alignment like RDC's RDCPCI5.SYS) that imports
# only from NDIS.SYS, the export table NDIS.VXD publishes on Windows 98.

CC32     = wcc386
CFLAGS32 = -q -bt=nt -mf -5s -ecd -zp8 -zl -s -wx -we -oaxt -d0

all : r6040.sys r6040dbg.exe .symbolic

r6040.obj : r6040.c ndis98.h r6040hw.h version.h
	$(CC32) $(CFLAGS32) r6040.c

r6040.sys : r6040.obj pefix.py
	wlink @<<r6040.lnk
format windows nt
runtime native=4.0
option quiet
option nodefaultlibs
option start='_DriverEntry@8'
option offset=0x10000
option objalign=512
option alignment=512
option stack=0x1000
option map=r6040.map
name r6040.sys
file r6040.obj
import '_NdisInitializeWrapper@16'                 'NDIS.SYS'.NdisInitializeWrapper
import '_NdisTerminateWrapper@8'                   'NDIS.SYS'.NdisTerminateWrapper
import '_NdisMRegisterMiniport@12'                 'NDIS.SYS'.NdisMRegisterMiniport
import '_NdisAllocateMemory@20'                    'NDIS.SYS'.NdisAllocateMemory
import '_NdisFreeMemory@12'                        'NDIS.SYS'.NdisFreeMemory
import '_NdisOpenConfiguration@12'                 'NDIS.SYS'.NdisOpenConfiguration
import '_NdisReadConfiguration@20'                 'NDIS.SYS'.NdisReadConfiguration
import '_NdisCloseConfiguration@4'                 'NDIS.SYS'.NdisCloseConfiguration
import '_NdisWriteConfiguration@16'                'NDIS.SYS'.NdisWriteConfiguration
import '_NdisMSetAttributes@16'                    'NDIS.SYS'.NdisMSetAttributes
import '_NdisMPciAssignResources@12'               'NDIS.SYS'.NdisMPciAssignResources
import '_NdisMQueryAdapterResources@16'            'NDIS.SYS'.NdisMQueryAdapterResources
import '_NdisReadPciSlotInformation@20'            'NDIS.SYS'.NdisReadPciSlotInformation
import '_NdisWritePciSlotInformation@20'           'NDIS.SYS'.NdisWritePciSlotInformation
import '_NdisMRegisterIoPortRange@16'              'NDIS.SYS'.NdisMRegisterIoPortRange
import '_NdisMDeregisterIoPortRange@16'            'NDIS.SYS'.NdisMDeregisterIoPortRange
import '_NdisMAllocateSharedMemory@20'             'NDIS.SYS'.NdisMAllocateSharedMemory
import '_NdisMFreeSharedMemory@24'                 'NDIS.SYS'.NdisMFreeSharedMemory
import '_NdisMRegisterInterrupt@28'                'NDIS.SYS'.NdisMRegisterInterrupt
import '_NdisMDeregisterInterrupt@4'               'NDIS.SYS'.NdisMDeregisterInterrupt
import '_NdisMRegisterAdapterShutdownHandler@12'   'NDIS.SYS'.NdisMRegisterAdapterShutdownHandler
import '_NdisMDeregisterAdapterShutdownHandler@4'  'NDIS.SYS'.NdisMDeregisterAdapterShutdownHandler
import '_NdisMSendResourcesAvailable@4'            'NDIS.SYS'.NdisMSendResourcesAvailable
import '_NdisMIndicateStatus@16'                   'NDIS.SYS'.NdisMIndicateStatus
import '_NdisMIndicateStatusComplete@4'            'NDIS.SYS'.NdisMIndicateStatusComplete
import '_NdisGetFirstBufferFromPacket@20'          'NDIS.SYS'.NdisGetFirstBufferFromPacket
import '_NdisQueryBuffer@12'                       'NDIS.SYS'.NdisQueryBuffer
<<
	python pefix.py r6040.sys

# DOS reader for the CMOS progress bytes (16-bit, real mode)
r6040dbg.exe : r6040dbg.c
	wcl -q -bt=dos -ms -0 -os -wx -fe=r6040dbg.exe r6040dbg.c

clean : .symbolic
	-rm -f *.obj *.map *.err *.lnk r6040.sys r6040dbg.exe
