/*
 * setrates - give the installed RDC M2010 (vesamini.drv) display adapter the
 * refresh-rate lists of the option ROM's timing table, so Windows 98 offers
 * them in Display Properties > Settings > Advanced > Adapter > Refresh rate.
 *
 * The same lists are in RDC-M2010.INF for new installs; this tool updates an
 * adapter that was installed from an older INF without reinstalling it.
 * It only writes the default value of existing MODES\<bpp>\<x,y> keys under
 * HKLM\System\CurrentControlSet\Services\Class\Display\<n> whose DEFAULT\drv
 * is vesamini.drv. Nothing else is changed.
 *
 *   setrates            update and report
 *   setrates file.txt   also write the report to file.txt
 *   setrates /undo      remove the rate lists again
 *
 * Build: wcl386 -q -bt=nt -l=nt setrates.c advapi32.lib
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static const char *class_path = "System\\CurrentControlSet\\Services\\Class\\Display";

static const struct { const char *res; const char *rates; } rom_rates[] = {
	{"640,480",   "60,72,75,85"},
	{"800,600",   "56,60,72,75,85"},
	{"1024,768",  "60,70,75,85"},
	{"1280,1024", "60"},
};
static const char *bpps[] = {"8", "16", "32"};

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

static BOOL read_str(HKEY root, const char *sub, const char *name, char *buf, DWORD size)
{
	HKEY k;
	DWORD type, len = size;
	buf[0] = '\0';
	if(RegOpenKeyEx(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return FALSE;
	if(RegQueryValueEx(k, name, NULL, &type, (BYTE*)buf, &len) != ERROR_SUCCESS || type != REG_SZ)
	{
		buf[0] = '\0';
		RegCloseKey(k);
		return FALSE;
	}
	RegCloseKey(k);
	return TRUE;
}

static int update_instance(HKEY display, const char *inst, BOOL undo)
{
	int i, j, done = 0;
	char path[256], cur[128];
	for(i = 0; i < 3; i++)
	{
		for(j = 0; j < (int)(sizeof(rom_rates) / sizeof(rom_rates[0])); j++)
		{
			HKEY k;
			sprintf(path, "%s\\MODES\\%s\\%s", inst, bpps[i], rom_rates[j].res);
			if(RegOpenKeyEx(display, path, 0, KEY_READ | KEY_WRITE, &k) != ERROR_SUCCESS)
			{
				say("  %-4s %-10s not in the mode list, skipped\n", bpps[i], rom_rates[j].res);
				continue;
			}
			read_str(display, path, "", cur, sizeof(cur));
			if(undo)
			{
				RegDeleteValue(k, "");
				say("  %-4s %-10s rates removed (were \"%s\")\n", bpps[i], rom_rates[j].res, cur);
			}
			else if(RegSetValueEx(k, "", 0, REG_SZ, (const BYTE*)rom_rates[j].rates,
			                      (DWORD)strlen(rom_rates[j].rates) + 1) == ERROR_SUCCESS)
			{
				say("  %-4s %-10s %s Hz (was \"%s\")\n", bpps[i], rom_rates[j].res, rom_rates[j].rates, cur);
				done++;
			}
			else
			{
				say("  %-4s %-10s could not be written\n", bpps[i], rom_rates[j].res);
			}
			RegCloseKey(k);
		}
	}
	return done;
}

int main(int argc, char **argv)
{
	HKEY display;
	DWORD idx;
	char inst[64], drv[128], desc[256], rr[64];
	BOOL undo = FALSE;
	int found = 0, i;

	for(i = 1; i < argc; i++)
	{
		if(stricmp(argv[i], "/undo") == 0) undo = TRUE;
		else out = fopen(argv[i], "w");
	}

	if(RegOpenKeyEx(HKEY_LOCAL_MACHINE, class_path, 0, KEY_READ, &display) != ERROR_SUCCESS)
	{
		say("setrates: cannot open HKLM\\%s\n", class_path);
		return 1;
	}

	for(idx = 0; ; idx++)
	{
		DWORD len = sizeof(inst);
		char sub[128];
		if(RegEnumKeyEx(display, idx, inst, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
		sprintf(sub, "%s\\DEFAULT", inst);
		read_str(display, sub, "drv", drv, sizeof(drv));
		read_str(display, inst, "DriverDesc", desc, sizeof(desc));
		if(stricmp(drv, "vesamini.drv") != 0)
		{
			say("Display\\%s: \"%s\" (drv %s) - not the M2010 driver, left alone\n", inst, desc, drv[0] ? drv : "-");
			continue;
		}
		found++;
		read_str(display, sub, "RefreshRate", rr, sizeof(rr));
		say("Display\\%s: \"%s\" (DEFAULT\\RefreshRate \"%s\")\n", inst, desc, rr);
		say("  %d mode(s) %s\n", update_instance(display, inst, undo), undo ? "cleared" : "updated");
	}
	RegCloseKey(display);

	if(read_str(HKEY_CURRENT_CONFIG, "Display\\Settings", "RefreshRate", rr, sizeof(rr)))
		say("Current RefreshRate setting (HKCC\\Display\\Settings): \"%s\"\n", rr);

	if(found == 0)
	{
		say("No display adapter using vesamini.drv was found; nothing changed.\n");
		return 2;
	}
	if(!undo)
		say("\nDone. Open Display Properties > Settings > Advanced > Adapter: the\n"
		    "Refresh rate list now offers the ROM's rates for the current mode.\n");
	if(out) fclose(out);
	return 0;
}
