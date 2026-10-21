/*****************************************************************************

Copyright (c) 2022  Michal Necasek
              2023  Jaroslav Hensl

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

#include "winhack.h"
#include <gdidefs.h>
#include <dibeng.h>
#include <minivdd.h>
#include <string.h>

#include "minidrv.h"
#include "cursor.h"

#include "3d_accel.h"

DWORD mouse_buf_lin = 0;
void __far* mouse_buf = NULL;
BOOL mouse_vxd = FALSE;

/*
 * What's this all about? Most of the required exported driver functions can
 * be passed straight to the DIB Engine. The DIB Engine in some cases requires
 * an additional parameter.
 *
 * See the dibthunk.asm module for functions that can be handled easily. Note
 * that although the logic could be implemented in C, it can be done more
 * efficiently in assembly. Forwarders turn into simple jumps, and functions
 * with an extra parameter can be implemented with very small overhead, saving
 * stack space and extra copying.
 *
 * This module deals with the very few functions that need additional logic but
 * are not hardware specific.
 */

#pragma code_seg( _TEXT )

void WINAPI __loadds MoveCursor(int absX, int absY)
{	
	if(wEnabled)
	{
		if(mouse_vxd)
		{
			mouse_move(absX, absY);
		}
		else
		{
			DIB_MoveCursorExt(absX, absY, lpDriverPDevice);
		}
	}
}

static size_t mouse_cursor_size(CURSORSHAPE __far *lpCursor)
{
	size_t s = 0;
	s = ((lpCursor->cx + 7)/8) * lpCursor->cy; // size of AND mask
	
	if(s == 0)
		return 0;	
	
	if(lpCursor->BitsPixel == 1)
	{
		s += ((lpCursor->cx + 7)/8) * lpCursor->cy; // size of XOR mask 1bpp
	}
	else
	{
		s += ((lpCursor->BitsPixel + 7)/8) * lpCursor->cx * lpCursor->cy; // size of XOR mask (mode bpp)
	}
	
	s += sizeof(CURSORSHAPE);
	
	return s;
}

WORD WINAPI __loadds SetCursor_driver(CURSORSHAPE __far *lpCursor)
{
	if(wEnabled)
	{
		if(mouse_vxd)
		{
			if(lpCursor != NULL)
			{
				size_t ms = mouse_cursor_size(lpCursor);
				if(ms > 0)
				{
					_fmemcpy(mouse_buf, lpCursor, ms);
					if(mouse_load())
					{
						return 1;
					}
					return 0;
				}
			}
			mouse_hide();
			return 1;
		}
		else
		{
			DIB_SetCursorExt(lpCursor, lpDriverPDevice);
			return 1;
		}
		
	}
	
	return 0;
}

/* Exported as DISPLAY.104 */
void WINAPI __loadds CheckCursor( void )
{
	if(wEnabled)
	{
		if(!mouse_vxd)
		{
			DIB_CheckCursorExt(lpDriverPDevice);
		}
	}
}

/* If there is no hardware screen-to-screen BitBlt, there's no point in
 * this and we can just forward BitBlt to the DIB Engine.
 */
#ifdef HWBLT

/* DIB engine physical brush header, identical for every depth (dibeng.inc) */
#pragma pack(push, 1)
typedef struct {
    BYTE  dpBrushFlags;   /* COLORSOLID, MONOSOLID, ... */
    BYTE  dpBrushBpp;
    WORD  dpBrushStyle;   /* BS_SOLID = 0 */
    DWORD dpFgColor;      /* physical foreground color */
    WORD  dpHatch;
    DWORD dpBgColor;
    BYTE  dpBrushMono[32];
    BYTE  dpBrushMask[32];
    BYTE  dpBrushBits[1];  /* 8x8 pattern in device pixels */
} DIB_BrushHdr;
#pragma pack(pop)

/* 8x8 pattern in device pixels for OP_M2010_PATBLT (DGROUP does not move) */
static BYTE m2010_pat_buf[8 * 8 * 4];

#define BS_HATCHED_ 2

/* Fill m2010_pat_buf for a pattern brush the engine can tile: colour pattern
 * brushes and dithered colours (dpBrushBits are the DIB engine's own device
 * pixels), and brushes made from a mono bitmap (PATTERNMONO: 0 bits take the
 * text colour, 1 bits the background colour, as GDI defines it). Hatched
 * brushes (transparent background mode, mask) stay with the DIB engine. */
static BOOL m2010_pattern(LPDIBENGINE lpDestDev, DIB_BrushHdr __far *brush, LPDRAWMODE lpDrawMode)
{
	WORD bpp = lpDestDev->deBitsPixel;
	WORD i, n;

	if(!(m2010_caps & M2010_CAP_PATFILL) || brush == NULL) return FALSE;
	if(lpDestDev->deFlags & PALETTE_XLAT) return FALSE;
	if(bpp != 8 && bpp != 16 && bpp != 32) return FALSE;
	if(brush->dpBrushStyle == BS_HATCHED_ || brush->dpBrushStyle == 1 /* BS_NULL */) return FALSE;
	if(brush->dpBrushBpp != (BYTE)bpp) return FALSE;

	n = (WORD)(bpp / 8);
	if(brush->dpBrushFlags & PATTERNMONO)
	{
		DWORD fg, bg;
		WORD x, y;
		if(lpDrawMode == NULL) return FALSE;
		fg = lpDrawMode->TextColor;
		bg = lpDrawMode->bkColor;
		for(y = 0; y < 8; y++)
		{
			BYTE bits = brush->dpBrushMono[y * 4];
			for(x = 0; x < 8; x++)
			{
				DWORD c = ((bits >> (7 - x)) & 1) ? bg : fg;
				BYTE *p = m2010_pat_buf + (y * 8 + x) * n;
				p[0] = (BYTE)c;
				if(n > 1) p[1] = (BYTE)(c >> 8);
				if(n > 2) { p[2] = (BYTE)(c >> 16); p[3] = 0; }
			}
		}
	}
	else
	{
		for(i = 0; i < 64 * n; i++)
			m2010_pat_buf[i] = brush->dpBrushBits[i];
	}
	return TRUE;
}

/* ROP3 truth table index is P*4 + S*2 + D */
#define ROP3_USES_SRC(_r)  ((((_r) >> 2) & 0x33) != ((_r) & 0x33))
#define ROP3_USES_PAT(_r)  ((((_r) >> 4) & 0x0F) != ((_r) & 0x0F))

/* Exported as DISPLAY.1
 * Solid fills (any ROP without a source) and screen-to-screen copies (any ROP
 * without a pattern) on the primary surface go to the RDC M2010 2D engine
 * through the VxD; everything else is done by the DIB engine.
 */
BOOL WINAPI __loadds BitBlt( LPDIBENGINE lpDestDev, WORD wDestX, WORD wDestY, LPPDEVICE lpSrcDev,
                    WORD wSrcX, WORD wSrcY, WORD wXext, WORD wYext, DWORD dwRop3,
                    LPBRUSH lpPBrush, LPDRAWMODE lpDrawMode )
{
	/* the VxD must own the software cursor so that FBHDA_access_rect() can
	   exclude it; with the DIB engine cursor we stay in software */
	if((m2010_caps & M2010_CAP_GDIBLT) && wEnabled && mouse_vxd && lpDestDev == lpDriverPDevice)
	{
		WORD dstFlags = lpDestDev->deFlags;

		if((dstFlags & VRAM) && !(dstFlags & BUSY) &&
			((DWORD)wXext * (DWORD)wYext) >= m2010_min_area)
		{
			BYTE  rop = (BYTE)(dwRop3 >> 16);
			DWORD dxy = ((DWORD)wDestX << 16) | wDestY;
			DWORD wh  = ((DWORD)wXext << 16) | wYext;

			if(!ROP3_USES_SRC(rop))
			{
				DWORD color = 0;
				BOOL  ok = TRUE;

				if(ROP3_USES_PAT(rop))
				{
					DIB_BrushHdr __far *brush = (DIB_BrushHdr __far *)lpPBrush;

					if(brush == NULL || !(brush->dpBrushFlags & COLORSOLID) ||
						brush->dpBrushStyle != BS_SOLID || (dstFlags & PALETTE_XLAT))
					{
						ok = FALSE;
					}
					else
					{
						/* the pattern bits already hold the colour as the DIB engine
						 * draws it, so engine fills match CPU fills exactly */
						switch(lpDestDev->deBitsPixel)
						{
							case 8:  color = brush->dpBrushBits[0]; break;
							case 16: color = *(WORD __far*)brush->dpBrushBits; break;
							default: color = *(DWORD __far*)brush->dpBrushBits; break;
						}
					}
				}

				if(ok && M2010_blt(lpDestDev->deBitsOffset, 0, dxy, wh, color, (WORD)rop | M2010_BLT_FILL))
				{
					return TRUE;
				}
				if(!ok && rop == 0xF0 && m2010_pattern(lpDestDev, (DIB_BrushHdr __far *)lpPBrush, lpDrawMode) &&
					M2010_patblt(lpDestDev->deBitsOffset, dxy, wh, (const void __far *)m2010_pat_buf, rop))
				{
					return TRUE;
				}
			}
			else if(!ROP3_USES_PAT(rop) && lpSrcDev == (LPPDEVICE)lpDestDev)
			{
				DWORD sxy = ((DWORD)wSrcX << 16) | wSrcY;

				if(M2010_blt(lpDestDev->deBitsOffset, sxy, dxy, wh, 0, (WORD)rop | M2010_BLT_COPY))
				{
					return TRUE;
				}
			}
		}
	}

	return DIB_BitBlt( lpDestDev, wDestX, wDestY, lpSrcDev, wSrcX, wSrcY, wXext, wYext, dwRop3, lpPBrush, lpDrawMode );
}

/* the screen PDEVICE in VRAM, free, no palette translation, engine allowed */
static BOOL m2010_screen_ok(LPDIBENGINE lpDestDev, DWORD cap)
{
	WORD f;
	if(!(m2010_caps & cap) || !wEnabled || !mouse_vxd || lpDestDev != lpDriverPDevice)
		return FALSE;
	f = lpDestDev->deFlags;
	return (f & VRAM) && !(f & (BUSY | PALETTE_XLAT));
}

/* a DIB engine physical colour (DRAWMODE.bkColor) is the device pixel in the
 * low bits; the top byte carries DIB engine flags */
static DWORD m2010_phys_pixel(LPDIBENGINE lpDestDev, DWORD c)
{
	switch(lpDestDev->deBitsPixel)
	{
		case 8:  return c & 0xFFUL;
		case 16: return c & 0xFFFFUL;
		default: return c & 0xFFFFFFUL;
	}
}

/* r = r AND c (exclusive right/bottom); FALSE if nothing is left */
static BOOL m2010_rect_and(RECT __far *r, const RECT __far *c)
{
	if(r->left   < c->left)   r->left   = c->left;
	if(r->top    < c->top)    r->top    = c->top;
	if(r->right  > c->right)  r->right  = c->right;
	if(r->bottom > c->bottom) r->bottom = c->bottom;
	return r->left < r->right && r->top < r->bottom;
}

static BOOL m2010_fill_rect(LPDIBENGINE lpDestDev, const RECT __far *r, DWORD pixel)
{
	DWORD dxy = ((DWORD)(WORD)r->left << 16) | (WORD)r->top;
	DWORD wh  = ((DWORD)(WORD)(r->right - r->left) << 16) | (WORD)(r->bottom - r->top);
	return M2010_blt(lpDestDev->deBitsOffset, 0, dxy, wh, pixel, 0xF0 | M2010_BLT_FILL);  /* PATCOPY */
}

/* Exported as DISPLAY.8
 * Rectangle() with no outline (NULL pen), a solid brush and R2_COPYPEN: the
 * interior goes to the engine; every other Output call to the DIB engine.
 */
#define R2_COPYPEN_ 13

WORD WINAPI __loadds Output( LPDIBENGINE lpDestDev, WORD wStyle, WORD wCount, LPPOINT lpPoints,
                             LPVOID lpPPen, LPVOID lpPBrush, LPDRAWMODE lpDrawMode, LPRECT lpClipRect )
{
	if(wStyle == OS_RECTANGLE && wCount >= 2 && lpPoints != NULL && lpPPen == NULL &&
		lpPBrush != NULL && lpDrawMode != NULL && lpDrawMode->Rop2 == R2_COPYPEN_ &&
		m2010_screen_ok(lpDestDev, M2010_CAP_RECTFILL))
	{
		DIB_BrushHdr __far *brush = (DIB_BrushHdr __far *)lpPBrush;

		if((brush->dpBrushFlags & COLORSOLID) && brush->dpBrushStyle == BS_SOLID)
		{
			RECT r, scr;
			DWORD pixel;

			/* without an outline GDI draws up to, not including, x2-1/y2-1 */
			r.left   = lpPoints[0].xcoord;
			r.top    = lpPoints[0].ycoord;
			r.right  = lpPoints[1].xcoord - 1;
			r.bottom = lpPoints[1].ycoord - 1;
			scr.left = 0; scr.top = 0;
			scr.right = lpDestDev->deWidth; scr.bottom = lpDestDev->deHeight;

			switch(lpDestDev->deBitsPixel)
			{
				case 8:  pixel = brush->dpBrushBits[0]; break;
				case 16: pixel = *(WORD __far*)brush->dpBrushBits; break;
				default: pixel = *(DWORD __far*)brush->dpBrushBits; break;
			}

			if(!m2010_rect_and((RECT __far *)&r, (RECT __far *)&scr) ||
				(lpClipRect != NULL && !m2010_rect_and((RECT __far *)&r, lpClipRect)))
			{
				return 1;   /* nothing visible */
			}
			if((DWORD)(r.right - r.left) * (DWORD)(r.bottom - r.top) >= m2010_min_area &&
				m2010_fill_rect(lpDestDev, (RECT __far *)&r, pixel))
			{
				return 1;
			}
		}
	}

	return DIB_Output( (LPPDEVICE)lpDestDev, wStyle, wCount, lpPoints, lpPPen, lpPBrush, lpDrawMode, lpClipRect );
}

#endif

#ifndef ETO_GLYPH_INDEX
#define ETO_GLYPH_INDEX 0x0010
#endif

#define ETO_OPAQUE_      0x0002
#define ETO_CLIPPED_     0x0004
#define ETO_PACKED_      0x0300   /* ETO_BYTE_PACKED | ETO_BIT_PACKED */
#define ETO_LEVEL_MODE_  0x1000
#define ORECT_MAX        16

/* Exported as DISPLAY.14
 * The opaque (background) rectangles go to the engine, the glyphs to the DIB
 * engine. How DIBENG's ExtTextOutExt treats them (Win98 DDK debug DIBENG.DLL,
 * SB_DrawOrects/SBN_OrectLoop/GetClip):
 *  - they are drawn when lpOpaqueRect != NULL and either ETO_OPAQUE is set or
 *    the string is in a packed glyph format (ETO_BYTE/BIT_PACKED);
 *  - packed: lpOpaqueRect is a list ended by an all-zero RECT, otherwise it
 *    is one RECT;
 *  - each is clipped to lpClipRect, and with ETO_CLIPPED the clip box is
 *    first narrowed to the first opaque rectangle.
 * After the fills the DIB engine gets lpOpaqueRect = NULL and, for
 * ETO_CLIPPED, the narrowed clip box, so it draws exactly the same glyphs.
 * Anything unusual goes to the DIB engine unchanged.
 */
DWORD WINAPI __loadds ExtTextOut( LPDIBENGINE lpDestDev, WORD wDestXOrg, WORD wDestYOrg, LPRECT lpClipRect,
                                        LPSTR lpString, int wCount, LPFONTINFO lpFontInfo, LPDRAWMODE lpDrawMode,
                                        LPTEXTXFORM lpTextXForm, LPSHORT lpCharWidths, LPRECT lpOpaqueRect, WORD wOptions )
{
#ifdef HWBLT
	if(wCount >= 0 && lpOpaqueRect != NULL && lpClipRect != NULL && lpDrawMode != NULL &&
		!(wOptions & ETO_LEVEL_MODE_) && (wOptions & (ETO_OPAQUE_ | ETO_PACKED_)) &&
		m2010_screen_ok(lpDestDev, M2010_CAP_TEXTFILL))
	{
		static RECT orects[ORECT_MAX];
		static RECT clip;
		int   n = 0, i;
		DWORD area = 0;
		DWORD pixel = m2010_phys_pixel(lpDestDev, lpDrawMode->bkColor);
		BOOL  packed = (wOptions & ETO_PACKED_) != 0;
		BOOL  ok = TRUE;

		clip = *lpClipRect;
		if(clip.left < 0) clip.left = 0;
		if(clip.top < 0)  clip.top = 0;
		if(clip.right  > (short)lpDestDev->deWidth)  clip.right  = lpDestDev->deWidth;
		if(clip.bottom > (short)lpDestDev->deHeight) clip.bottom = lpDestDev->deHeight;
		if((wOptions & ETO_CLIPPED_) && !m2010_rect_and(&clip, lpOpaqueRect))
		{
			ok = FALSE;   /* empty: the DIB engine draws nothing either */
		}

		for(i = 0; ok; i++)
		{
			const RECT __far *src = lpOpaqueRect + i;
			if(packed && src->left == 0 && src->top == 0 && src->right == 0 && src->bottom == 0)
				break;
			if(i >= ORECT_MAX) { ok = FALSE; break; }
			orects[n] = *src;
			if(m2010_rect_and(&orects[n], &clip))
			{
				area += (DWORD)(orects[n].right - orects[n].left) * (DWORD)(orects[n].bottom - orects[n].top);
				n++;
			}
			if(!packed) break;
		}

		if(ok && area >= m2010_min_area)
		{
			for(i = 0; i < n; i++)
			{
				if(!m2010_fill_rect(lpDestDev, &orects[i], pixel))
				{
					break;
				}
			}
			if(i == n)
			{
				if(wCount == 0)
				{
					return 0;   /* rectangles only, like DIBENG's success return */
				}
				return DIB_ExtTextOut((LPPDEVICE)lpDestDev, wDestXOrg, wDestYOrg, (LPRECT)&clip, lpString, wCount,
					lpFontInfo, lpDrawMode, lpTextXForm, lpCharWidths, NULL,
					(WORD)(wOptions & ~(ETO_OPAQUE_ | ETO_CLIPPED_)));
			}
			/* an engine fill failed: the DIB engine repaints everything below */
		}
	}
#endif
	return DIB_ExtTextOut(lpDestDev, wDestXOrg, wDestYOrg, lpClipRect, lpString, wCount, lpFontInfo, lpDrawMode, lpTextXForm, lpCharWidths, lpOpaqueRect, wOptions);
}

BOOL WINAPI __loadds DDIGammaRamp(LPDIBENGINE lpDev, BOOL fGetSet, LPARAM lpGammaRamp)
{
	BOOL rc;
	if(fGetSet)
	{
		rc = FBHDA_gamma_set((VOID FBPTR)lpGammaRamp, sizeof(WORD)*256*3);
		dbg_printf("set the ramp: %d\n", rc);
	}
	else
	{
		rc = FBHDA_gamma_get((VOID FBPTR)lpGammaRamp, sizeof(WORD)*256*3);
		dbg_printf("get the ramp\n");
	}
	
	return rc;
}
