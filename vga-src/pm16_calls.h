#ifndef __VXDCALL_H__INCLUDED__
#define __VXDCALL_H__INCLUDED__

BOOL VXD_VM_connect();
void M2010_drvlog_flush();

#ifdef QEMU
BOOL VXD_QEMUFX_supported();
#endif

#endif /* __VXDCALL_H__INCLUDED__ */
