/*
 * m2010log - dump the in-memory debug log and the M2010 status of the
 * vesamini.vxd display driver (DBGMEM build) on Windows 9x.
 *
 *   m2010log            prints the log to stdout
 *   m2010log > log.txt  saves it
 *
 * Build: wcl386 -q -bt=nt -l=nt m2010log.c
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#define OP_M2010_INFO 0x4100
#define OP_M2010_LOG  0x4102

static char logbuf[65536];

/* Win98's COMMAND.COM loses the output of a redirected Win32 console program
 * inside an "if" line, so the tool can write the file itself: m2010log file.txt */
static FILE *out = NULL;

static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	if(out)
	{
		va_start(ap, fmt);
		vfprintf(out, fmt, ap);
		va_end(ap);
	}
}

int main(int argc, char **argv)
{
	HANDLE h;
	DWORD got = 0;
	DWORD info[2] = {0, 0};

	if(argc > 1)
	{
		out = fopen(argv[1], "wb");
		if(out == NULL)
		{
			printf("cannot create %s\n", argv[1]);
		}
	}

	h = CreateFile("\\\\.\\vesamini.vxd", 0, 0, NULL, 0, FILE_FLAG_DELETE_ON_CLOSE, NULL);
	if(h == INVALID_HANDLE_VALUE)
	{
		h = CreateFile("\\\\.\\VESAMINI", 0, 0, NULL, 0, FILE_FLAG_DELETE_ON_CLOSE, NULL);
	}
	if(h == INVALID_HANDLE_VALUE)
	{
		say("cannot open vesamini.vxd (error %lu) - is the RDC M2010 display driver active?\n", GetLastError());
		return 1;
	}

	if(DeviceIoControl(h, OP_M2010_INFO, NULL, 0, info, sizeof(info), &got, NULL) && got >= 8)
	{
		say("M2010 caps: %08lX (%s%s%s) min GDI blit area %lu px\n", info[0],
			(info[0] & 1) ? "present " : "not-found ",
			(info[0] & 2) ? "engine-on " : "engine-off ",
			(info[0] & 4) ? "gdi-blt" : "gdi-sw",
			info[1]);
	}
	else
	{
		say("OP_M2010_INFO not answered (error %lu) - not the M2010 driver build?\n", GetLastError());
	}

	got = 0;
	if(!DeviceIoControl(h, OP_M2010_LOG, NULL, 0, logbuf, sizeof(logbuf), &got, NULL))
	{
		say("OP_M2010_LOG failed (error %lu) - this is not the debug (DBGMEM) build.\n", GetLastError());
		CloseHandle(h);
		return 2;
	}
	say("---- vesamini.vxd log (%lu bytes) ----\n", got);
	fwrite(logbuf, 1, got, stdout);
	if(out) fwrite(logbuf, 1, got, out);
	say("\n---- end of log ----\n");
	CloseHandle(h);
	if(out) fclose(out);
	return 0;
}
