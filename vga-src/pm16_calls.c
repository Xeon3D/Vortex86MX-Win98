/*****************************************************************************

Copyright (c) 2023 Jaroslav Hensl <emulator@emulace.cz>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

*****************************************************************************/
/* Communication from ring3 PM16 -> ring0 PM32 */

#include <wchar.h> /* wchar_t */
#include <string.h> /* _fmemset */
#include <stdint.h>
#include "winhack.h" /* BOOL */

#include "dpmi.h"

#include <gdidefs.h>
#include <dibeng.h>
#include <valmode.h>
#include <minivdd.h>
#include "minidrv.h"

#include "pm16_calls.h"
#include "vxd.h"
#include "3d_accel.h"

#ifdef DBGPRINT
extern void dbg_printf( const char *s, ... );
#else
#define dbg_printf(...)
#endif

#ifndef SVGA
# ifndef VESA
#  define VBE
# endif
#endif

static DWORD VXD_VM = 0;
static DWORD vxd_fbhda16 = 0;
static DWORD vxd_fbhda32 = 0;
static DWORD vxd_mouse16  = 0;

#pragma code_seg( _INIT )

BOOL VXD_VM_connect()
{
	if(VXD_VM != 0)
	{
		return TRUE;
	}

	/* read service vector */
	_asm{
		push es
		push ax
		push dx
		push bx
		push di
		
		xor di,di
		mov es,di
		mov ax, 1684H
		mov bx, VXD_DEVICE_ID
		int 2FH
		mov word ptr [VXD_VM],di
		mov word ptr [VXD_VM+2],es
		
		pop di
		pop bx
		pop dx
		pop ax
		pop es
	};

	dbg_printf("VDD_REGISTER_DISPLAY_DRIVER_INFO for VM: %d\n", OurVMHandle);

	_asm{
		.386
    push   eax
    push   ebx
    push   ecx
    push   edx
    push   esi
    
    mov    eax, VDD_REGISTER_DISPLAY_DRIVER_INFO
    movzx  ebx, word ptr [OurVMHandle]
    call dword ptr [VDDEntryPoint]
    
    mov  [vxd_fbhda16], edx
    mov  [vxd_mouse16], ecx
    mov  [vxd_fbhda32], esi
    
    pop    esi
    pop    edx
    pop    ecx
    pop    ebx
    pop    eax
  }
  
  dbg_printf("VDD_REGISTER_DISPLAY_DRIVER_INFO: eax=%lX, edx=%lX, ecx=%lX\n", VXD_VM, vxd_fbhda16, vxd_mouse16);
#ifdef VESA
	M2010_drvlog_flush();
#endif
  
	return VXD_VM != 0;
}

void FBHDA_setup(FBHDA_t __far* __far* FBHDA, DWORD __far* FBHDA_linear)
{
	*FBHDA = (FBHDA_t __far*)vxd_fbhda16;
	*FBHDA_linear = vxd_fbhda32;
}

void FBHDA_access_begin(DWORD flags)
{
	static DWORD sFlags;
	sFlags = flags;
	
	_asm
	{
		.386
		push eax
		push edx
		push ecx
		
	  mov edx, OP_FBHDA_ACCESS_BEGIN
	  mov ecx, [sFlags]
	  call dword ptr [VXD_VM]
	  
	  pop ecx
		pop edx
		pop eax
	}
}

void FBHDA_access_end(DWORD flags)
{
	static DWORD sFlags;
	sFlags = flags;
	
	_asm
	{
		.386
		push eax
		push edx
		push ecx
		
	  mov edx, OP_FBHDA_ACCESS_END
	  mov ecx, [sFlags]
	  call dword ptr [VXD_VM]
	  
	  pop ecx
		pop edx
		pop eax
	}
}

void FBHDA_access_rect(DWORD left, DWORD top, DWORD right, DWORD bottom)
{
	static DWORD sL;
	static DWORD sTop;
	static DWORD sR;
	static DWORD sB;
	
	sL = left;
	sTop = top;
	sR = right;
	sB = bottom;
	
	_asm
	{
		.386
		push eax
		push edx
		push ebx
		push ecx
		push esi
		push edi
		
	  mov edx, OP_FBHDA_ACCESS_RECT
	  mov ebx, [sL]
	  mov ecx, [sTop]
	  mov esi, [sR]
	  mov edi, [sB]
	  call dword ptr [VXD_VM]
	  
	  pop edi
	  pop esi
	  pop ecx
	  pop ebx
	  pop edx
		pop eax
	}
}

BOOL FBHDA_swap(DWORD offset)
{
	static DWORD sOffset;
	static BOOL status;
	sOffset = offset;
	status = FALSE;
	
	_asm
	{
		.386
		push eax
		push edx
		push ecx
		
	  mov edx, OP_FBHDA_SWAP
	  mov ecx, [sOffset]
	  call dword ptr [VXD_VM]
	  mov [status], cx
	  
	  pop ecx
		pop edx
		pop eax
	}
	
	return status == 0 ? FALSE : TRUE;
}

void FBHDA_clean()
{
	_asm
	{
		.386
		push eax
		push edx
		
	  mov edx, OP_FBHDA_CLEAN
	  call dword ptr [VXD_VM]
	  
		pop edx
		pop eax
	}
}

void FBHDA_palette_set(unsigned char index, DWORD rgb)
{
	static unsigned char sIndex;
	static DWORD sRGB;
	
	sIndex = index;
	sRGB = rgb;
	
	_asm
	{
		.386
		push eax
		push edx
		push ecx
		push edi
		
	  mov edx, OP_FBHDA_PALETTE_SET
	  mov cl, [index]
	  mov edi, [sRGB]
	  call dword ptr [VXD_VM]
	  
	  pop edi
	  pop ecx
		pop edx
		pop eax
	}
}

DWORD FBHDA_palette_get(unsigned char index)
{
	static unsigned char sIndex;
	static DWORD sRGB;
	
	sIndex = index;
	sRGB = 0;
	
	_asm
	{
		.386
		push eax
		push edx
		push ecx
		push edi
		
	  mov edx, OP_FBHDA_PALETTE_GET
	  mov cl, [index]
	  call dword ptr [VXD_VM]
	  mov [sRGB], edi
	  
	  pop edi
	  pop ecx
		pop edx
		pop eax
	}
	
	return sRGB;
}

BOOL FBHDA_gamma_get(VOID FBPTR ramp, DWORD buffer_size)
{
	static DWORD ramp_linear;
	static DWORD ramp_size;
	static unsigned short status;
	
	ramp_linear = DPMI_GetSegBase(((DWORD)ramp) >> 16);
	ramp_linear += ((DWORD)ramp) & 0xFFFFUL;
	
	ramp_size = buffer_size;
	
	_asm
	{
		.386
		push eax
		push edx
		push ecx
		push edi
		
		mov edx, OP_FBHDA_GAMMA_GET
		mov ecx, [buffer_size]
		mov edi, [ramp_linear]
		call dword ptr [VXD_VM]
		mov [status],cx
		
		pop edi
		pop ecx
		pop edx
		pop eax
	}
	
	return status == 0 ? FALSE : TRUE;
}

BOOL FBHDA_gamma_set(VOID FBPTR ramp, DWORD buffer_size)
{
	static DWORD ramp_linear;
	static DWORD ramp_size;
	static unsigned short status;
	
	ramp_linear = DPMI_GetSegBase(((DWORD)ramp) >> 16);
	ramp_linear += ((DWORD)ramp) & 0xFFFFUL;
	
	ramp_size = buffer_size;
	
	_asm
	{
		.386
		push eax
		push edx
		push ecx
		push esi
		
		mov edx, OP_FBHDA_GAMMA_SET
		mov ecx, [buffer_size]
		mov esi, [ramp_linear]
		call dword ptr [VXD_VM]
		mov [status],cx
		
		pop esi
		pop ecx
		pop edx
		pop eax
	}
	
	return status == 0 ? FALSE : TRUE;
}

BOOL mouse_load()
{
	static BOOL status;
	
	_asm
	{
		.386
		push eax
		push edx
		push ecx
		
		mov edx, OP_MOUSE_LOAD
		call dword ptr [VXD_VM]
		mov [status],cx
		
		pop ecx
		pop edx
		pop eax
	}
	
	return status == 0 ? FALSE : TRUE;
}

void mouse_buffer(void __far* __far* pBuf, DWORD __far* pLinear)
{
	*pBuf = (void __far*)vxd_mouse16;
}

void mouse_move(int x, int y)
{
	static DWORD sx;
	static DWORD sy;
	
	sx = x;
	sy = y;
	
	_asm
	{
		.386
		push eax
		push edx
		push esi
		push edi
		
		mov esi, [sx]
		mov edi, [sy]
		mov edx, OP_MOUSE_MOVE
		call dword ptr [VXD_VM]
		
		pop edi
		pop esi
		pop edx
		pop eax
	}
}

void mouse_show()
{
	_asm
	{
		.386
		push eax
		push edx
		
		mov edx, OP_MOUSE_SHOW
		call dword ptr [VXD_VM]
		
		pop edx
		pop eax
	}
}

void mouse_hide()
{
	_asm
	{
		.386
		push eax
		push edx
		
		mov edx, OP_MOUSE_HIDE
		call dword ptr [VXD_VM]
		
		pop edx
		pop eax
	}
}


#ifdef SVGA
BOOL SVGA_valid()
{
	static BOOL status;
	status = FALSE;
	
	_asm
	{
		.386
		push eax
		push edx
		push ecx
	  
	  mov edx, OP_SVGA_VALID
	  call dword ptr [VXD_VM]
	  mov  [status], cx
	  
	  pop ecx
		pop edx
		pop eax
	}
	
	return status == 0 ? FALSE : TRUE;
}

BOOL SVGA_setmode(DWORD w, DWORD h, DWORD bpp)
{
	static BOOL status;
	static DWORD sw, sh, sbpp;
	
	status = FALSE;
	sw = w;
	sh = h;
	sbpp = bpp;
	
	_asm
	{
		.386
		push eax
		push edx
		push ecx
		push esi
		push edi
	  
	  mov edx, OP_SVGA_SETMODE
	  mov ecx, [bpp]
	  mov esi, [w]
	  mov edi, [h]
	  call dword ptr [VXD_VM]
	  mov  [status], cx
	  
	  pop edi
	  pop esi
	  pop ecx
		pop edx
		pop eax
	}
	
	return status == 0 ? FALSE : TRUE;
}

BOOL SVGA_validmode(DWORD w, DWORD h, DWORD bpp)
{
	static BOOL status;
	static DWORD sw, sh, sbpp;
	
	status = FALSE;
	sw = w;
	sh = h;
	sbpp = bpp;
	
	_asm
	{
		.386
		push eax
		push edx
		push ecx
		push esi
		push edi
	  
	  mov edx, OP_SVGA_VALIDMODE
	  mov ecx, [bpp]
	  mov esi, [w]
	  mov edi, [h]
	  call dword ptr [VXD_VM]
	  mov  [status], cx
	  
	  pop edi
	  pop esi
	  pop ecx
		pop edx
		pop eax
	}
	
	return status == 0 ? FALSE : TRUE;
}

void SVGA_HW_enable()
{
	_asm
	{
		.386
		push eax
		push edx
		
	  mov edx, OP_SVGA_HW_ENABLE
	  call dword ptr [VXD_VM]
	  
		pop edx
		pop eax
	}
}

void SVGA_HW_disable()
{
	_asm
	{
		.386
		push eax
		push edx
		
	  mov edx, OP_SVGA_HW_DISABLE
	  call dword ptr [VXD_VM]
	  
		pop edx
		pop eax
	}
}
#endif

#ifdef VBE
BOOL VBE_valid()
{
	static BOOL status;
	status = FALSE;
	
	_asm
	{
		.386
		push eax
		push edx
		push ecx
	  
	  mov edx, OP_VBE_VALID
	  call dword ptr [VXD_VM]
	  mov  [status], cx
	  
	  pop ecx
		pop edx
		pop eax
	}
	
	return status == 0 ? FALSE : TRUE;
}

BOOL VBE_setmode(DWORD w, DWORD h, DWORD bpp)
{
	static BOOL status;
	static DWORD sw, sh, sbpp;
	
	status = FALSE;
	sw = w;
	sh = h;
	sbpp = bpp;
	
	_asm
	{
		.386
		push eax
		push edx
		push ecx
		push esi
		push edi
	  
	  mov edx, OP_VBE_SETMODE
	  mov ecx, [bpp]
	  mov esi, [w]
	  mov edi, [h]
	  call dword ptr [VXD_VM]
	  mov  [status], cx
	  
	  pop edi
	  pop esi
	  pop ecx
		pop edx
		pop eax
	}
	
	return status == 0 ? FALSE : TRUE;
}

BOOL VBE_validmode(DWORD w, DWORD h, DWORD bpp)
{
	static BOOL status;
	static DWORD sw, sh, sbpp;
	
	status = FALSE;
	sw = w;
	sh = h;
	sbpp = bpp;
	
	_asm
	{
		.386
		push eax
		push edx
		push ecx
		push esi
		push edi
	  
	  mov edx, OP_VBE_VALIDMODE
	  mov ecx, [bpp]
	  mov esi, [w]
	  mov edi, [h]
	  call dword ptr [VXD_VM]
	  mov  [status], cx
	  
	  pop edi
	  pop esi
	  pop ecx
		pop edx
		pop eax
	}
	
	return status == 0 ? FALSE : TRUE;
}

#endif /* VBE */


#ifdef VESA
BOOL VESA_valid()
{
	static BOOL status;
	status = FALSE;
	
	_asm
	{
		.386
		push eax
		push edx
		push ecx
	  
	  mov edx, OP_VESA_VALID
	  call dword ptr [VXD_VM]
	  mov  [status], cx
	  
	  pop ecx
		pop edx
		pop eax
	}
	
	return status == 0 ? FALSE : TRUE;
}

BOOL VESA_setmode(DWORD w, DWORD h, DWORD bpp, DWORD rr_min, DWORD rr_max)
{
	static BOOL status;
	static DWORD s_wh, s_rr, sbpp;
	
	status = FALSE;
	s_wh = (w << 16) | h;
	sbpp = bpp;
	s_rr = (rr_min << 16) | rr_max;
	
	_asm
	{
		.386
		push eax
		push edx
		push ecx
		push esi
		push edi
	  
	  mov edx, OP_VESA_SETMODE
	  mov ecx, [bpp]
	  mov esi, [s_wh]
	  mov edi, [s_rr]
	  call dword ptr [VXD_VM]
	  mov  [status], cx
	  
	  pop edi
	  pop esi
	  pop ecx
		pop edx
		pop eax
	}
	
	return status == 0 ? FALSE : TRUE;
}

static void VESA_HIRES(DWORD hires_on)
{
	static DWORD s_hires_on;

	s_hires_on = hires_on;

	_asm
	{
		.386
		push eax
		push edx
		push ecx
	  
	  mov edx, OP_VESA_HIRES
	  mov ecx, [s_hires_on]
	  call dword ptr [VXD_VM]
	  
	  pop ecx
		pop edx
		pop eax
	}
}

void VESA_HIRES_enable()
{
	VESA_HIRES(1);
}

void VESA_HIRES_disable()
{
	VESA_HIRES(0);
}

BOOL VESA_validmode(DWORD w, DWORD h, DWORD bpp)
{
	static BOOL status;
	static DWORD sw, sh, sbpp;
	
	status = FALSE;
	sw = w;
	sh = h;
	sbpp = bpp;
	
	_asm
	{
		.386
		push eax
		push edx
		push ecx
		push esi
		push edi
	  
	  mov edx, OP_VESA_VALIDMODE
	  mov ecx, [bpp]
	  mov esi, [w]
	  mov edi, [h]
	  call dword ptr [VXD_VM]
	  mov  [status], cx
	  
	  pop edi
	  pop esi
	  pop ecx
		pop edx
		pop eax
	}
	
	return status == 0 ? FALSE : TRUE;
}

#endif /* VESA */

#ifdef VESA
/* RDC M2010 2D engine queries/requests (vesamini.vxd) */
DWORD M2010_info(DWORD __far *min_area)
{
	static DWORD s_caps;
	static DWORD s_area;

	s_caps = 0;
	s_area = 0;

	_asm
	{
		.386
		push eax
		push edx
		push ecx
		push esi

		mov edx, OP_M2010_INFO
		xor ecx, ecx
		xor esi, esi
		call dword ptr [VXD_VM]
		mov [s_caps], ecx
		mov [s_area], esi

		pop esi
		pop ecx
		pop edx
		pop eax
	}

	if(min_area != NULL && s_area != 0)
	{
		*min_area = s_area;
	}
	return s_caps;
}

/*
 * surf     = byte offset of the GDI surface in VRAM (PDEVICE deBitsOffset)
 * sxy/dxy  = x << 16 | y
 * wh       = width << 16 | height
 * ropflags = ROP3 byte | M2010_BLT_FILL or M2010_BLT_COPY
 */
BOOL M2010_blt(DWORD surf, DWORD sxy, DWORD dxy, DWORD wh, DWORD color, WORD ropflags)
{
	static DWORD s_surf;
	static DWORD s_sxy;
	static DWORD s_dxy;
	static DWORD s_wh;
	static DWORD s_color;
	static DWORD s_op;
	static WORD  status;

	s_surf  = surf;
	s_sxy   = sxy;
	s_dxy   = dxy;
	s_wh    = wh;
	s_color = color;
	s_op    = ((DWORD)ropflags << 16) | OP_M2010_BLT;
	status  = 0;

	_asm
	{
		.386
		push eax
		push edx
		push ecx
		push esi
		push edi
		push ebx

		mov edx, [s_op]
		mov eax, [s_color]
		mov ebx, [s_dxy]
		mov ecx, [s_wh]
		mov esi, [s_sxy]
		mov edi, [s_surf]
		call dword ptr [VXD_VM]
		mov [status], cx

		pop ebx
		pop edi
		pop esi
		pop ecx
		pop edx
		pop eax
	}

	return status == 0 ? FALSE : TRUE;
}
/*
 * PATCOPY with an 8x8 colour pattern: pat = 64 device pixels (rows of 8)
 * in a buffer that does not move (the driver's DGROUP); the VxD reads it
 * through its linear address.
 */
BOOL M2010_patblt(DWORD surf, DWORD dxy, DWORD wh, const void __far *pat, WORD rop)
{
	static WORD  s_sel = 0;
	static DWORD s_base = 0;
	static DWORD s_surf;
	static DWORD s_dxy;
	static DWORD s_wh;
	static DWORD s_pat;
	static DWORD s_op;
	static WORD  status;

	if(s_sel != (WORD)(((DWORD)pat) >> 16))
	{
		s_sel  = (WORD)(((DWORD)pat) >> 16);
		s_base = DPMI_GetSegBase(s_sel);
	}
	s_surf = surf;
	s_dxy  = dxy;
	s_wh   = wh;
	s_pat  = s_base + (((DWORD)pat) & 0xFFFFUL);
	s_op   = ((DWORD)rop << 16) | OP_M2010_PATBLT;
	status = 0;

	_asm
	{
		.386
		push eax
		push edx
		push ecx
		push esi
		push edi
		push ebx

		mov edx, [s_op]
		mov ebx, [s_dxy]
		mov ecx, [s_wh]
		mov esi, [s_pat]
		mov edi, [s_surf]
		call dword ptr [VXD_VM]
		mov [status], cx

		pop ebx
		pop edi
		pop esi
		pop ecx
		pop edx
		pop eax
	}

	return status == 0 ? FALSE : TRUE;
}
#endif /* VESA */

#ifdef VESA
/* 16-bit debug output -> VxD in-memory log (DBGMEM16 builds) */
static char drvlog_pending[1024];
static int  drvlog_pending_len = 0;
static DWORD logvm = 0; /* VxD API entry found by the logger itself */

void M2010_drvlog(const char __far *s)
{
	static DWORD lr[4];
	static DWORD entry;

	if(VXD_VM == 0 && logvm == 0)
	{
		/* no connection yet: look the VxD up ourselves (int 2F/1684h) */
		_asm{
			push es
			push ax
			push bx
			push di
			xor di,di
			mov es,di
			mov ax, 1684H
			mov bx, VXD_DEVICE_ID
			int 2FH
			mov word ptr [logvm],di
			mov word ptr [logvm+2],es
			pop di
			pop bx
			pop ax
			pop es
		}
	}
	entry = VXD_VM ? VXD_VM : logvm;

	if(entry == 0)
	{
		/* keep the text until a VxD entry point is known */
		while(*s && drvlog_pending_len < sizeof(drvlog_pending) - 1)
		{
			drvlog_pending[drvlog_pending_len++] = *s++;
		}
		drvlog_pending[drvlog_pending_len] = 0;
		return;
	}

	/* 16 bytes per call in EBX:ECX:ESI:EDI, zero padded; no pointers, so
	 * nothing has to be mapped on the ring-0 side */
	while(*s)
	{
		int i;
		char *b = (char*)lr;
		for(i = 0; i < 16; i++)
		{
			b[i] = *s;
			if(*s) s++;
		}
		_asm
		{
			.386
			push eax
			push edx
			push ebx
			push ecx
			push esi
			push edi
			mov edx, OP_M2010_DRVLOG
			mov ebx, [lr]
			mov ecx, [lr+4]
			mov esi, [lr+8]
			mov edi, [lr+12]
			call dword ptr [entry]
			pop edi
			pop esi
			pop ecx
			pop ebx
			pop edx
			pop eax
		}
	}
}

void M2010_drvlog_flush()
{
	if((VXD_VM != 0 || logvm != 0) && drvlog_pending_len > 0)
	{
		static char copy[1024];
		int i;
		for(i = 0; i <= drvlog_pending_len; i++) copy[i] = drvlog_pending[i];
		drvlog_pending_len = 0;
		M2010_drvlog(copy);
	}
}
#endif /* VESA */
