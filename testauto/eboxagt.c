/*
 * eboxagt - unattended test agent for the eBOX-3300MX (Windows 98 SE).
 *
 * Runs hidden from HKLM\...\Run. Keeps one FTP control connection to the PC's
 * exchange server (exchange/ftpserver.py) and:
 *   - polls /out/JOB/JOB.TXT (MDTM+SIZE, which the server does not log) and
 *     runs a job whose "SEQ n" is newer than the last one it ran;
 *   - uploads a heartbeat /in/HB.TXT every 60 s and after each job;
 *   - writes C:\M2010\BOOTOK.TXT once it has run for 60 s (for BOOTGUARD).
 * Results go to /in/RUN/<seq>/ (RESULT.TXT is re-uploaded after every step).
 * There is no listening socket: the agent only connects out to the PC.
 * <dir>\OFF.TXT (INSTAGT OFF) keeps it from starting.
 * Tray icon: red = no connection to the PC, green = idle, yellow = running a job;
 * double-click = status, right-click = Status / Open log / Stop agent.
 *
 * Job file (one command per line, '#' comments):
 *   SEQ <n>                      must come first
 *   RUN [/T=sec] <command line>  COMSPEC /c <cmd> > OUT<step>.TXT, uploaded
 *   GET <remote> <local>         download (remote is relative to the FTP root)
 *   PUT <local> [<name>]         upload into /in/RUN/<seq>/
 *   STAT [<file> ...]            sizes + md5 of the driver files (+ extra), CUR*.TXT,
 *                                WININIT.INI -> STAT.TXT, uploaded
 *   SHOT <name>                  desktop as 24-bit <name>.BMP, uploaded
 *   QUEUE <dst> <src>            add a WININIT.INI [rename] line (dst=src)
 *   WAIT <sec>
 *   RESTART [FORCE] / SHUTDOWN [FORCE]   must be the last step
 *   QUIT                         stop the agent
 *
 * Options: /pc=10.0.0.239 /port=21 /dir=C:\M2010\AGT /work=C:\M2010
 *          /poll=5 /hb=60 /active (skip PASV) /dry (RESTART/SHUTDOWN simulated)
 *
 * Build: wcl386 -q -bt=nt -l=nt_win -bm eboxagt.c wsock32.lib shell32.lib
 */
#include <windows.h>
#include <shellapi.h>
#include <winsock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <process.h>

#define AGT_VERSION "1.1"

static char pc_addr[64] = "10.0.0.239";
static int pc_port = 21;
static char agt_dir[MAX_PATH] = "C:\\M2010\\AGT";
static char work_dir[MAX_PATH] = "C:\\M2010";
static int poll_s = 5, hb_s = 60, dry = 0, force_active = 0;

static HWND hwnd_main;
static long boot_num;
static DWORD start_tick;
static char start_time[32];
static long last_seq;
static char state[96] = "idle";

/* ------------------------------------------------------------------ util */

static void now_str(char *buf)
{
	SYSTEMTIME t;
	GetLocalTime(&t);
	sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay,
		t.wHour, t.wMinute, t.wSecond);
}

static void path_in(char *buf, const char *name)
{
	sprintf(buf, "%s\\%s", agt_dir, name);
}

static void agt_log(const char *fmt, ...)
{
	char p[MAX_PATH], ts[32];
	FILE *f;
	va_list ap;

	path_in(p, "AGT.LOG");
	f = fopen(p, "a");
	if (f == NULL)
		return;
	if (ftell(f) > 256 * 1024L) {
		char old[MAX_PATH];
		fclose(f);
		path_in(old, "AGT.OLD");
		DeleteFile(old);
		MoveFile(p, old);
		f = fopen(p, "a");
		if (f == NULL)
			return;
	}
	now_str(ts);
	fprintf(f, "%s ", ts);
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

static long read_num(const char *name)
{
	char p[MAX_PATH];
	long n = 0;
	FILE *f;

	path_in(p, name);
	f = fopen(p, "r");
	if (f != NULL) {
		if (fscanf(f, "%ld", &n) != 1)
			n = 0;
		fclose(f);
	}
	return n;
}

static void write_num(const char *name, long n)
{
	char p[MAX_PATH];
	FILE *f;

	path_in(p, name);
	f = fopen(p, "w");
	if (f != NULL) {
		fprintf(f, "%ld\n", n);
		fclose(f);
	}
}

static char *trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t'))
		*--e = 0;
	return s;
}

/* split off the first whitespace-separated token; returns the rest */
static char *token(char *s, char **tok)
{
	while (*s == ' ' || *s == '\t')
		s++;
	*tok = s;
	while (*s && *s != ' ' && *s != '\t')
		s++;
	if (*s)
		*s++ = 0;
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

static const char *base_name(const char *p)
{
	const char *b = p, *s;
	for (s = p; *s; s++)
		if (*s == '\\' || *s == '/' || *s == ':')
			b = s + 1;
	return b;
}

/* ------------------------------------------------------------------ md5 (RFC 1321) */

typedef struct { DWORD s[4]; DWORD n[2]; unsigned char b[64]; } md5_t;

#define F1(x, y, z) (z ^ (x & (y ^ z)))
#define F2(x, y, z) F1(z, x, y)
#define F3(x, y, z) (x ^ y ^ z)
#define F4(x, y, z) (y ^ (x | ~z))

static void md5_block(DWORD *st, const unsigned char *p)
{
	static const DWORD k[64] = {
		0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
		0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
		0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
		0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
		0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
		0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
		0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
		0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
	static const int r[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};
	DWORD m[16], a = st[0], b = st[1], c = st[2], d = st[3], t;
	int i, g;

	for (i = 0; i < 16; i++)
		m[i] = p[i * 4] | (DWORD)p[i * 4 + 1] << 8 | (DWORD)p[i * 4 + 2] << 16 | (DWORD)p[i * 4 + 3] << 24;
	for (i = 0; i < 64; i++) {
		DWORD f;
		if (i < 16) {
			f = F1(b, c, d); g = i;
		} else if (i < 32) {
			f = F2(b, c, d); g = (5 * i + 1) & 15;
		} else if (i < 48) {
			f = F3(b, c, d); g = (3 * i + 5) & 15;
		} else {
			f = F4(b, c, d); g = (7 * i) & 15;
		}
		t = d;
		d = c;
		c = b;
		f += a + k[i] + m[g];
		b += f << r[i >> 4][i & 3] | f >> (32 - r[i >> 4][i & 3]);
		a = t;
	}
	st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

static void md5_init(md5_t *m)
{
	m->s[0] = 0x67452301; m->s[1] = 0xefcdab89; m->s[2] = 0x98badcfe; m->s[3] = 0x10325476;
	m->n[0] = m->n[1] = 0;
}

static void md5_add(md5_t *m, const unsigned char *p, unsigned len)
{
	unsigned used = m->n[0] & 63;
	if ((m->n[0] += len) < len)
		m->n[1]++;
	while (len) {
		unsigned c = 64 - used;
		if (c > len)
			c = len;
		memcpy(m->b + used, p, c);
		used += c; p += c; len -= c;
		if (used == 64) {
			md5_block(m->s, m->b);
			used = 0;
		}
	}
}

static void md5_hex(md5_t *m, char *out)
{
	static const unsigned char pad[64] = {0x80};
	unsigned char len[8];
	DWORD lo = m->n[0] << 3, hi = m->n[1] << 3 | m->n[0] >> 29;
	int i;

	for (i = 0; i < 4; i++) {
		len[i] = (unsigned char)(lo >> (8 * i));
		len[i + 4] = (unsigned char)(hi >> (8 * i));
	}
	md5_add(m, pad, ((m->n[0] & 63) < 56 ? 56 : 120) - (m->n[0] & 63));
	md5_add(m, len, 8);
	for (i = 0; i < 16; i++)
		sprintf(out + 2 * i, "%02x", (unsigned)(m->s[i >> 2] >> (8 * (i & 3))) & 0xff);
}

/* "size md5" of a file, or "missing" */
static void file_sum(const char *p, char *out)
{
	static unsigned char buf[8192];
	FILE *f = fopen(p, "rb");
	md5_t m;
	long size = 0;
	size_t n;
	char hex[33];

	if (f == NULL) {
		strcpy(out, "missing");
		return;
	}
	md5_init(&m);
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		md5_add(&m, buf, (unsigned)n);
		size += (long)n;
	}
	fclose(f);
	md5_hex(&m, hex);
	sprintf(out, "%ld %s", size, hex);
}

/* ------------------------------------------------------------------ ftp client */

static SOCKET ctl = INVALID_SOCKET;
static char rbuf[2048];
static int rlen;
static char reply[512];
static int pasv_ok = 1;

static SOCKET connect_to(unsigned long ip, int port, int ms)
{
	SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a;
	u_long nb = 1;
	fd_set wf, ef;
	struct timeval tv;

	if (s == INVALID_SOCKET)
		return s;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = ip;
	a.sin_port = htons((u_short)port);
	ioctlsocket(s, FIONBIO, &nb);
	if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0 && WSAGetLastError() != WSAEWOULDBLOCK)
		goto fail;
	FD_ZERO(&wf); FD_SET(s, &wf);
	FD_ZERO(&ef); FD_SET(s, &ef);
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	if (select(0, NULL, &wf, &ef, &tv) <= 0 || FD_ISSET(s, &ef) || !FD_ISSET(s, &wf))
		goto fail;
	nb = 0;
	ioctlsocket(s, FIONBIO, &nb);
	return s;
fail:
	closesocket(s);
	return INVALID_SOCKET;
}

static int wait_read(SOCKET s, int ms)
{
	fd_set rf;
	struct timeval tv;
	FD_ZERO(&rf);
	FD_SET(s, &rf);
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	return select(0, &rf, NULL, NULL, &tv) > 0;
}

static void ctl_close(void)
{
	if (ctl != INVALID_SOCKET)
		closesocket(ctl);
	ctl = INVALID_SOCKET;
	rlen = 0;
}

static int read_line(char *line, int size, int ms)
{
	for (;;) {
		char *nl = memchr(rbuf, '\n', rlen);
		int n;
		if (nl != NULL) {
			int l = (int)(nl - rbuf) + 1;
			int c = l < size ? l : size - 1;
			memcpy(line, rbuf, c);
			line[c] = 0;
			memmove(rbuf, rbuf + l, rlen - l);
			rlen -= l;
			trim(line);
			return 0;
		}
		if (rlen == sizeof(rbuf))
			rlen = 0; /* overlong line: drop it */
		if (!wait_read(ctl, ms))
			return -1;
		n = recv(ctl, rbuf + rlen, sizeof(rbuf) - rlen, 0);
		if (n <= 0)
			return -1;
		rlen += n;
	}
}

/* read a (multi-line) reply; returns the code or -1 (connection closed) */
static int ctl_reply(int ms)
{
	char line[512];
	int code;

	if (read_line(line, sizeof(line), ms) != 0)
		goto dead;
	code = atoi(line);
	strcpy(reply, line);
	if (strlen(line) > 3 && line[3] == '-') {
		for (;;) {
			if (read_line(line, sizeof(line), ms) != 0)
				goto dead;
			if (atoi(line) == code && line[3] == ' ')
				break;
		}
	}
	return code;
dead:
	ctl_close();
	return -1;
}

static int ctl_cmd(const char *fmt, ...)
{
	char buf[600];
	va_list ap;
	int n;

	if (ctl == INVALID_SOCKET)
		return -1;
	va_start(ap, fmt);
	n = vsprintf(buf, fmt, ap);
	va_end(ap);
	strcpy(buf + n, "\r\n");
	if (send(ctl, buf, n + 2, 0) != n + 2) {
		ctl_close();
		return -1;
	}
	return ctl_reply(30000);
}

static int ftp_connect(void)
{
	unsigned long ip;

	if (ctl != INVALID_SOCKET)
		return 0;
	ip = inet_addr(pc_addr);
	ctl = connect_to(ip, pc_port, 5000);
	if (ctl == INVALID_SOCKET)
		return -1;
	rlen = 0;
	if (ctl_reply(10000) != 220 || ctl_cmd("USER anonymous") != 331 ||
	    ctl_cmd("PASS ebox@") != 230 || ctl_cmd("TYPE I") != 200) {
		ctl_close();
		return -1;
	}
	agt_log("connected to %s:%d", pc_addr, pc_port);
	return 0;
}

/* open the data connection. PASV first; PORT if PASV fails (e.g. firewall). */
static SOCKET data_prepare(SOCKET *lst)
{
	*lst = INVALID_SOCKET;
	if (pasv_ok && !force_active && ctl_cmd("PASV") == 227) {
		int h1, h2, h3, h4, p1, p2;
		char *p = strchr(reply, '(');
		if (p != NULL && sscanf(p + 1, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) == 6) {
			SOCKET d = connect_to(inet_addr(pc_addr), p1 * 256 + p2, 5000);
			if (d != INVALID_SOCKET)
				return d;
		}
		agt_log("PASV data connection failed, using PORT from now on");
		pasv_ok = 0;
	}
	if (ctl == INVALID_SOCKET)
		return INVALID_SOCKET;
	{
		struct sockaddr_in a;
		int al = sizeof(a);
		unsigned char *ip, *pt;
		SOCKET l;

		getsockname(ctl, (struct sockaddr *)&a, &al);
		a.sin_port = 0;
		l = socket(AF_INET, SOCK_STREAM, 0);
		if (l == INVALID_SOCKET)
			return l;
		if (bind(l, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(l, 1) != 0) {
			closesocket(l);
			return INVALID_SOCKET;
		}
		al = sizeof(a);
		getsockname(l, (struct sockaddr *)&a, &al);
		ip = (unsigned char *)&a.sin_addr.s_addr;
		pt = (unsigned char *)&a.sin_port;
		if (ctl_cmd("PORT %d,%d,%d,%d,%d,%d", ip[0], ip[1], ip[2], ip[3], pt[0], pt[1]) != 200) {
			closesocket(l);
			return INVALID_SOCKET;
		}
		*lst = l;
		return INVALID_SOCKET;
	}
}

/* after the 150 reply: accept the PORT connection */
static SOCKET data_accept(SOCKET d, SOCKET lst)
{
	SOCKET s;
	if (d != INVALID_SOCKET || lst == INVALID_SOCKET)
		return d;
	s = INVALID_SOCKET;
	if (wait_read(lst, 15000))
		s = accept(lst, NULL, NULL);
	closesocket(lst);
	return s;
}

static int xfer(const char *verb, const char *remote, const char *local)
{
	static char buf[16384];
	SOCKET d, lst;
	FILE *f;
	int code, ok = 1, get = verb[0] == 'R';

	if (ftp_connect() != 0)
		return -1;
	f = fopen(local, get ? "wb" : "rb");
	if (f == NULL) {
		agt_log("%s %s: cannot open %s", verb, remote, local);
		return -1;
	}
	d = data_prepare(&lst);
	if (d == INVALID_SOCKET && lst == INVALID_SOCKET) {
		fclose(f);
		return -1;
	}
	code = ctl_cmd("%s %s", verb, remote);
	if (code != 150 && code != 125) {
		if (d != INVALID_SOCKET)
			closesocket(d);
		if (lst != INVALID_SOCKET)
			closesocket(lst);
		fclose(f);
		if (get)
			DeleteFile(local);
		return code < 0 ? -1 : code;
	}
	d = data_accept(d, lst);
	if (d == INVALID_SOCKET) {
		ok = 0;
	} else if (get) {
		for (;;) {
			int n;
			if (!wait_read(d, 60000)) {
				ok = 0;
				break;
			}
			n = recv(d, buf, sizeof(buf), 0);
			if (n < 0)
				ok = 0;
			if (n <= 0)
				break;
			if (fwrite(buf, 1, n, f) != (size_t)n)
				ok = 0;
		}
	} else {
		size_t n;
		while (ok && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
			char *p = buf;
			while (n > 0) {
				int s = send(d, p, (int)n, 0);
				if (s <= 0) {
					ok = 0;
					break;
				}
				p += s;
				n -= s;
			}
		}
	}
	if (d != INVALID_SOCKET)
		closesocket(d);
	fclose(f);
	code = ctl_reply(60000);
	if (!ok || code != 226) {
		agt_log("%s %s failed (ok=%d reply %d)", verb, remote, ok, code);
		if (get)
			DeleteFile(local);
		return code == 226 ? -1 : code;
	}
	return 0;
}

static int ftp_get(const char *remote, const char *local) { return xfer("RETR", remote, local); }
static int ftp_put(const char *local, const char *remote) { return xfer("STOR", remote, local); }

/* change key of a remote file: "mdtm size", "" if it does not exist */
static int ftp_key(const char *remote, char *key)
{
	int c;
	key[0] = 0;
	if (ftp_connect() != 0)
		return -1;
	c = ctl_cmd("MDTM %s", remote);
	if (c < 0)
		return -1;
	if (c != 213)
		return 0;
	strcpy(key, reply + 4);
	c = ctl_cmd("SIZE %s", remote);
	if (c < 0)
		return -1;
	if (c == 213) {
		strcat(key, " ");
		strcat(key, reply + 4);
	}
	return 0;
}

/* ------------------------------------------------------------------ heartbeat */

static void heartbeat(void)
{
	char p[MAX_PATH], ts[32];
	FILE *f;

	path_in(p, "HB.TXT");
	f = fopen(p, "w");
	if (f == NULL)
		return;
	now_str(ts);
	fprintf(f, "agent=EBOXAGT " AGT_VERSION "\nboot=%ld\nstarted=%s\nnow=%s\nuptime=%lu\n"
		"agent_uptime=%lu\nlastseq=%ld\nstate=%s\ndata=%s\ndry=%d\n",
		boot_num, start_time, ts, GetTickCount() / 1000, (GetTickCount() - start_tick) / 1000,
		last_seq, state, pasv_ok && !force_active ? "pasv" : "port", dry);
	fclose(f);
	ftp_put(p, "/in/HB.TXT");
}

/* ------------------------------------------------------------------ job steps */

static long cur_seq;
static char res_path[MAX_PATH];

static void res(const char *fmt, ...)
{
	FILE *f = fopen(res_path, "a");
	va_list ap;
	if (f == NULL)
		return;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

static int put_run(const char *local, const char *name)
{
	char r[MAX_PATH];
	sprintf(r, "/in/RUN/%ld/%s", cur_seq, name);
	return ftp_put(local, r);
}

static void upload_result(void)
{
	put_run(res_path, "RESULT.TXT");
}

static int do_run(char *args, int step)
{
	char cmd[700], out[MAX_PATH], name[16];
	const char *comspec = getenv("COMSPEC");
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	DWORD rc = 0, w;
	int tmo = 600;

	if (strnicmp(args, "/T=", 3) == 0) {
		char *t;
		args = token(args, &t);
		tmo = atoi(t + 3);
	}
	sprintf(name, "OUT%d.TXT", step);
	path_in(out, name);
	DeleteFile(out);
	if (comspec == NULL)
		comspec = "command.com";
	sprintf(cmd, "%s /c %s > %s", comspec, args, out);
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_SHOWMINNOACTIVE;
	if (!CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, work_dir, &si, &pi)) {
		res("  cannot start: error %lu", GetLastError());
		return -1;
	}
	w = WaitForSingleObject(pi.hProcess, (DWORD)tmo * 1000);
	if (w == WAIT_TIMEOUT) {
		TerminateProcess(pi.hProcess, 255);
		res("  timeout after %d s, terminated", tmo);
		rc = (DWORD)-2;
	} else {
		GetExitCodeProcess(pi.hProcess, &rc);
	}
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	if (GetFileAttributes(out) != 0xFFFFFFFF)
		put_run(out, name);
	return (int)rc;
}

static int do_stat(char *args)
{
	static const char *files[] = {
		"C:\\WINDOWS\\SYSTEM\\VESAMINI.DRV", "C:\\WINDOWS\\SYSTEM\\VESAMINI.VXD",
		"C:\\WINDOWS\\SYSTEM\\RDCPCI5.SYS", "C:\\WINDOWS\\SYSTEM\\R6040.SYS", NULL};
	static const char *texts[] = {
		"C:\\M2010\\B\\CURV.TXT", "C:\\M2010\\B\\CURN.TXT", "C:\\WINDOWS\\WININIT.INI",
		"C:\\WINDOWS\\WININIT.BAK", "C:\\M2010\\ROLLBACK.TXT", NULL};
	char p[MAX_PATH], sum[64], ts[32], line[256];
	FILE *f, *t;
	int i;

	path_in(p, "STAT.TXT");
	f = fopen(p, "w");
	if (f == NULL)
		return -1;
	now_str(ts);
	fprintf(f, "now=%s\nboot=%ld\nuptime=%lu\n", ts, boot_num, GetTickCount() / 1000);
	for (i = 0; files[i] != NULL; i++) {
		file_sum(files[i], sum);
		fprintf(f, "file %s %s\n", files[i], sum);
	}
	while (*args) {
		char *tok;
		args = token(args, &tok);
		file_sum(tok, sum);
		fprintf(f, "file %s %s\n", tok, sum);
	}
	for (i = 0; texts[i] != NULL; i++) {
		t = fopen(texts[i], "r");
		if (t == NULL)
			continue;
		fprintf(f, "text %s\n", texts[i]);
		while (fgets(line, sizeof(line), t) != NULL)
			fprintf(f, "| %s\n", trim(line));
		fclose(t);
	}
	fclose(f);
	return put_run(p, "STAT.TXT");
}

static int do_shot(const char *name)
{
	HDC sdc, mdc;
	HBITMAP bm, old;
	BITMAPINFOHEADER bi;
	BITMAPFILEHEADER fh;
	int w, h, stride, rc = -1;
	unsigned char *bits;
	char p[MAX_PATH], fn[64];
	FILE *f;

	sdc = GetDC(NULL);
	w = GetSystemMetrics(SM_CXSCREEN);
	h = GetSystemMetrics(SM_CYSCREEN);
	mdc = CreateCompatibleDC(sdc);
	bm = CreateCompatibleBitmap(sdc, w, h);
	old = SelectObject(mdc, bm);
	BitBlt(mdc, 0, 0, w, h, sdc, 0, 0, SRCCOPY);
	SelectObject(mdc, old);
	stride = (w * 3 + 3) & ~3;
	bits = malloc((size_t)stride * h);
	memset(&bi, 0, sizeof(bi));
	bi.biSize = sizeof(bi);
	bi.biWidth = w;
	bi.biHeight = h;
	bi.biPlanes = 1;
	bi.biBitCount = 24;
	bi.biCompression = BI_RGB;
	if (bits != NULL && GetDIBits(mdc, bm, 0, h, bits, (BITMAPINFO *)&bi, DIB_RGB_COLORS) == h) {
		sprintf(fn, "%.8s.BMP", name);
		path_in(p, fn);
		f = fopen(p, "wb");
		if (f != NULL) {
			memset(&fh, 0, sizeof(fh));
			fh.bfType = 0x4D42;
			fh.bfOffBits = sizeof(fh) + sizeof(bi);
			fh.bfSize = fh.bfOffBits + stride * h;
			bi.biSizeImage = stride * h;
			fwrite(&fh, sizeof(fh), 1, f);
			fwrite(&bi, sizeof(bi), 1, f);
			fwrite(bits, stride, h, f);
			fclose(f);
			rc = put_run(p, fn);
			res("  %dx%d -> %s", w, h, fn);
		}
	}
	free(bits);
	DeleteObject(bm);
	DeleteDC(mdc);
	ReleaseDC(NULL, sdc);
	return rc;
}

static int do_queue(char *args)
{
	char *dst, *src, line[256];
	const char *ini = "C:\\WINDOWS\\WININIT.INI";
	int has = 0;
	FILE *f;

	src = token(args, &dst);
	if (*dst == 0 || *src == 0)
		return -1;
	f = fopen(ini, "r");
	if (f != NULL) {
		while (fgets(line, sizeof(line), f) != NULL)
			if (strnicmp(trim(line), "[rename]", 8) == 0)
				has = 1;
		fclose(f);
	}
	f = fopen(ini, "a");
	if (f == NULL)
		return -1;
	if (!has)
		fprintf(f, "[rename]\n");
	fprintf(f, "%s=%s\n", dst, src);
	fclose(f);
	return 0;
}

static void finish(int how, int force)
{
	UINT f = how ? EWX_REBOOT : EWX_SHUTDOWN;
	if (force)
		f |= EWX_FORCE;
	if (dry) {
		/* pretend: wait, then behave like a fresh boot */
		agt_log("dry run: would %s", how ? "restart" : "shut down");
		Sleep(5000);
		boot_num++;
		write_num("BOOTNUM.TXT", boot_num);
		start_tick = GetTickCount();
		now_str(start_time);
		return;
	}
	agt_log("%s", how ? "restart" : "shutdown");
	ctl_close();
	if (!ExitWindowsEx(f, 0))
		agt_log("ExitWindowsEx failed: %lu", GetLastError());
	Sleep(INFINITE);
}

static void run_job(const char *job)
{
	char line[600], ts[32];
	FILE *f = fopen(job, "r");
	int step = 0, fin = -1, force = 0;
	DWORD t0;

	if (f == NULL)
		return;
	cur_seq = last_seq;
	path_in(res_path, "RESULT.TXT");
	DeleteFile(res_path);
	now_str(ts);
	ctl_cmd("MKD /in/RUN");
	ctl_cmd("MKD /in/RUN/%ld", cur_seq);
	res("SEQ %ld", cur_seq);
	res("START %s boot=%ld uptime=%lu agent=" AGT_VERSION, ts, boot_num, GetTickCount() / 1000);
	agt_log("job %ld start", cur_seq);
	while (fin < 0 && fgets(line, sizeof(line), f) != NULL) {
		char *verb, *args, *l = trim(line);
		int rc = 0;
		if (*l == 0 || *l == '#' || strnicmp(l, "SEQ", 3) == 0)
			continue;
		step++;
		res("STEP %d %s", step, l);
		sprintf(state, "job %ld step %d", cur_seq, step);
		upload_result();
		args = token(l, &verb);
		t0 = GetTickCount();
		if (stricmp(verb, "RUN") == 0) {
			rc = do_run(args, step);
		} else if (stricmp(verb, "GET") == 0) {
			char *r, *loc;
			loc = token(args, &r);
			rc = ftp_get(r, loc);
		} else if (stricmp(verb, "PUT") == 0) {
			char *loc, *name = token(args, &loc);
			rc = put_run(loc, *name ? name : base_name(loc));
		} else if (stricmp(verb, "STAT") == 0) {
			rc = do_stat(args);
		} else if (stricmp(verb, "SHOT") == 0) {
			rc = do_shot(*args ? args : "SHOT");
		} else if (stricmp(verb, "QUEUE") == 0) {
			rc = do_queue(args);
		} else if (stricmp(verb, "WAIT") == 0) {
			Sleep((DWORD)atoi(args) * 1000);
		} else if (stricmp(verb, "RESTART") == 0 || stricmp(verb, "SHUTDOWN") == 0) {
			fin = toupper(verb[0]) == 'R';
			force = stricmp(args, "FORCE") == 0;
		} else if (stricmp(verb, "QUIT") == 0) {
			fin = 2;
		} else {
			rc = -1;
			res("  unknown command");
		}
		res("  rc=%d t=%lu ms", rc, GetTickCount() - t0);
	}
	fclose(f);
	now_str(ts);
	res("END %s %s", fin == 1 ? "restart" : fin == 0 ? "shutdown" : fin == 2 ? "quit" : "ok", ts);
	upload_result();
	strcpy(state, "idle");
	agt_log("job %ld end", cur_seq);
	if (fin == 0 || fin == 1)
		finish(fin, force);
	if (fin == 2) {
		heartbeat();
		ctl_cmd("QUIT");
		PostMessage(hwnd_main, WM_CLOSE, 0, 0);
		Sleep(INFINITE);
	}
	heartbeat();
}

/* ------------------------------------------------------------------ worker */

static void worker(void *arg)
{
	char key[80], last_key[80] = "", job[MAX_PATH], line[64];
	DWORD last_hb = 0;
	int bootok = 0, hb_due = 1;

	(void)arg;
	path_in(job, "JOB.TXT");
	for (;;) {
		if (!bootok && GetTickCount() - start_tick > 60000) {
			FILE *f = fopen("C:\\M2010\\BOOTOK.TXT", "w");
			if (f != NULL) {
				fprintf(f, "%ld\n", boot_num);
				fclose(f);
			}
			bootok = 1;
		}
		if (ftp_connect() != 0) {
			Sleep(poll_s * 1000);
			continue;
		}
		if (hb_due || GetTickCount() - last_hb >= (DWORD)hb_s * 1000) {
			heartbeat();
			last_hb = GetTickCount();
			hb_due = 0;
		}
		if (ftp_key("/out/JOB/JOB.TXT", key) == 0 && key[0] && strcmp(key, last_key) != 0 &&
		    ftp_get("/out/JOB/JOB.TXT", job) == 0) {
			FILE *f = fopen(job, "r");
			long seq = 0;
			strcpy(last_key, key);
			if (f != NULL) {
				while (fgets(line, sizeof(line), f) != NULL) {
					char *l = trim(line);
					if (*l == 0 || *l == '#')
						continue;
					if (strnicmp(l, "SEQ ", 4) == 0)
						seq = atol(l + 4);
					break;
				}
				fclose(f);
			}
			if (seq > last_seq) {
				last_seq = seq;
				write_num("LASTSEQ.TXT", last_seq);
				run_job(job);
				last_hb = GetTickCount();
				continue;
			}
		}
		Sleep(poll_s * 1000);
	}
}

/* ------------------------------------------------------------------ tray icon */

#define WM_TRAY     (WM_APP + 1)
#define TRAY_TIMER  1
#define NID_V1_SIZE 88 /* NOTIFYICONDATA as the Win98 shell knows it (64-char tip) */
#define IDM_STATUS  100
#define IDM_LOG     101
#define IDM_STOP    102

static HICON tray_icons[3]; /* red = no PC connection, green = idle, yellow = job running */
static int tray_shown, tray_cur = -1;
static char tray_tip[64];
static UINT msg_taskbar_created;

static HICON make_icon(COLORREF c)
{
	HDC sdc = GetDC(NULL), dc = CreateCompatibleDC(sdc);
	ICONINFO ii;
	HBITMAP old;
	HBRUSH br = CreateSolidBrush(c);
	HICON ic;

	ii.fIcon = TRUE;
	ii.xHotspot = ii.yHotspot = 0;
	ii.hbmColor = CreateCompatibleBitmap(sdc, 16, 16);
	ii.hbmMask = CreateBitmap(16, 16, 1, 1, NULL);
	/* mask: white = transparent, black disc = opaque */
	old = SelectObject(dc, ii.hbmMask);
	PatBlt(dc, 0, 0, 16, 16, WHITENESS);
	SelectObject(dc, GetStockObject(BLACK_BRUSH));
	SelectObject(dc, GetStockObject(BLACK_PEN));
	Ellipse(dc, 1, 1, 15, 15);
	/* colour: black outside the disc, coloured disc with a black rim */
	SelectObject(dc, ii.hbmColor);
	PatBlt(dc, 0, 0, 16, 16, BLACKNESS);
	SelectObject(dc, br);
	Ellipse(dc, 1, 1, 15, 15);
	SelectObject(dc, GetStockObject(WHITE_BRUSH));
	SelectObject(dc, old);
	ic = CreateIconIndirect(&ii);
	DeleteObject(br);
	DeleteObject(ii.hbmColor);
	DeleteObject(ii.hbmMask);
	DeleteDC(dc);
	ReleaseDC(NULL, sdc);
	return ic;
}

/* refresh icon + tooltip when the state changed; (re)adds the icon if Explorer lost it */
static void tray_update(int force)
{
	NOTIFYICONDATA nid;
	char st[96], tip[160];
	int s;

	strncpy(st, state, sizeof(st) - 1);
	st[sizeof(st) - 1] = 0;
	s = ctl == INVALID_SOCKET ? 0 : strcmp(st, "idle") != 0 ? 2 : 1;
	sprintf(tip, "EBOXAGT " AGT_VERSION " - %s, last job %ld", s == 0 ? "no PC connection" : st, last_seq);
	tip[63] = 0;
	if (!force && tray_shown && s == tray_cur && strcmp(tip, tray_tip) == 0)
		return;
	memset(&nid, 0, sizeof(nid));
	nid.cbSize = NID_V1_SIZE;
	nid.hWnd = hwnd_main;
	nid.uID = 1;
	nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
	nid.uCallbackMessage = WM_TRAY;
	nid.hIcon = tray_icons[s];
	strcpy(nid.szTip, tip);
	if (Shell_NotifyIcon(tray_shown ? NIM_MODIFY : NIM_ADD, &nid)) {
		if (!tray_shown)
			agt_log("tray icon shown");
		tray_shown = 1;
		tray_cur = s;
		strcpy(tray_tip, tip);
	} else {
		tray_shown = 0; /* no taskbar (yet): add again on the next tick */
	}
}

static void tray_remove(void)
{
	NOTIFYICONDATA nid;
	memset(&nid, 0, sizeof(nid));
	nid.cbSize = NID_V1_SIZE;
	nid.hWnd = hwnd_main;
	nid.uID = 1;
	Shell_NotifyIcon(NIM_DELETE, &nid);
	tray_shown = 0;
}

static void tray_status(void)
{
	char msg[600];
	sprintf(msg, "EBOXAGT " AGT_VERSION "\n\nPC: %s:%d (%s, data %s)\nState: %s\nLast job: %ld\n"
		"Boot: %ld, Windows up %lu min, agent up %lu min\nFolder: %s%s",
		pc_addr, pc_port, ctl == INVALID_SOCKET ? "not connected" : "connected",
		pasv_ok && !force_active ? "PASV" : "PORT", state, last_seq, boot_num,
		GetTickCount() / 60000, (GetTickCount() - start_tick) / 60000, agt_dir,
		dry ? "\nDry run: RESTART/SHUTDOWN are simulated" : "");
	MessageBox(hwnd_main, msg, "eBOX test agent", MB_OK | MB_ICONINFORMATION);
}

static void tray_menu(void)
{
	HMENU m = CreatePopupMenu();
	POINT pt;
	int cmd;

	AppendMenu(m, MF_STRING, IDM_STATUS, "Status...");
	AppendMenu(m, MF_STRING, IDM_LOG, "Open log");
	AppendMenu(m, MF_SEPARATOR, 0, NULL);
	AppendMenu(m, MF_STRING, IDM_STOP, "Stop agent");
	SetMenuDefaultItem(m, IDM_STATUS, FALSE);
	GetCursorPos(&pt);
	SetForegroundWindow(hwnd_main); /* or the menu does not close when clicking elsewhere */
	cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_main, NULL);
	PostMessage(hwnd_main, WM_NULL, 0, 0);
	DestroyMenu(m);
	if (cmd == IDM_STATUS) {
		tray_status();
	} else if (cmd == IDM_LOG) {
		char p[MAX_PATH], c[MAX_PATH + 16];
		path_in(p, "AGT.LOG");
		sprintf(c, "notepad.exe %s", p);
		WinExec(c, SW_SHOWNORMAL);
	} else if (cmd == IDM_STOP) {
		if (MessageBox(hwnd_main, "Stop the test agent?\n\nIt starts again at the next logon "
			"(INSTAGT OFF switches it off for good).", "eBOX test agent",
			MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
			agt_log("stopped from the tray menu");
			DestroyWindow(hwnd_main);
		}
	}
}

/* ------------------------------------------------------------------ main */

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	if (m == msg_taskbar_created && m != 0) {
		tray_shown = 0; /* Explorer restarted */
		tray_update(1);
		return 0;
	}
	switch (m) {
	case WM_TIMER:
		tray_update(0);
		return 0;
	case WM_TRAY:
		if (l == WM_RBUTTONUP)
			tray_menu();
		else if (l == WM_LBUTTONDBLCLK)
			tray_status();
		return 0;
	case WM_QUERYENDSESSION:
		return TRUE;
	case WM_ENDSESSION:
		if (w)
			agt_log("session ending");
		return 0;
	case WM_DESTROY:
		KillTimer(h, TRAY_TIMER);
		tray_remove();
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(h, m, w, l);
}

static void parse_args(char *cl)
{
	while (*cl) {
		char *t;
		cl = token(cl, &t);
		if (strnicmp(t, "/pc=", 4) == 0)
			strncpy(pc_addr, t + 4, sizeof(pc_addr) - 1);
		else if (strnicmp(t, "/port=", 6) == 0)
			pc_port = atoi(t + 6);
		else if (strnicmp(t, "/dir=", 5) == 0)
			strncpy(agt_dir, t + 5, sizeof(agt_dir) - 1);
		else if (strnicmp(t, "/work=", 6) == 0)
			strncpy(work_dir, t + 6, sizeof(work_dir) - 1);
		else if (strnicmp(t, "/poll=", 6) == 0)
			poll_s = atoi(t + 6);
		else if (strnicmp(t, "/hb=", 4) == 0)
			hb_s = atoi(t + 4);
		else if (stricmp(t, "/dry") == 0)
			dry = 1;
		else if (stricmp(t, "/active") == 0)
			force_active = 1;
	}
	if (poll_s < 1)
		poll_s = 1;
	if (hb_s < 5)
		hb_s = 5;
}

int PASCAL WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
	static char cl[512];
	WNDCLASS wc;
	WSADATA wsa;
	MSG msg;

	(void)prev;
	(void)show;
	strncpy(cl, cmdline, sizeof(cl) - 1);
	parse_args(cl);
	CreateMutex(NULL, TRUE, "EBOXAGT_SINGLE");
	if (GetLastError() == ERROR_ALREADY_EXISTS)
		return 0;
	CreateDirectory("C:\\M2010", NULL);
	CreateDirectory(agt_dir, NULL);
	{
		char off[MAX_PATH];
		path_in(off, "OFF.TXT");
		if (GetFileAttributes(off) != 0xFFFFFFFF)
			return 0; /* switched off by INSTAGT OFF */
	}
	start_tick = GetTickCount();
	now_str(start_time);
	boot_num = read_num("BOOTNUM.TXT") + 1;
	write_num("BOOTNUM.TXT", boot_num);
	last_seq = read_num("LASTSEQ.TXT");
	agt_log("EBOXAGT " AGT_VERSION " start boot=%ld lastseq=%ld pc=%s:%d dry=%d", boot_num,
		last_seq, pc_addr, pc_port, dry);
	if (WSAStartup(0x0101, &wsa) != 0) {
		agt_log("WSAStartup failed");
		return 1;
	}
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = inst;
	wc.lpszClassName = "EBOXAGT";
	RegisterClass(&wc);
	hwnd_main = CreateWindow("EBOXAGT", "EBOXAGT", 0, 0, 0, 0, 0, NULL, NULL, inst, NULL);
	msg_taskbar_created = RegisterWindowMessage("TaskbarCreated");
	tray_icons[0] = make_icon(RGB(210, 0, 0));
	tray_icons[1] = make_icon(RGB(0, 180, 0));
	tray_icons[2] = make_icon(RGB(245, 200, 0));
	tray_update(1);
	SetTimer(hwnd_main, TRAY_TIMER, 1000, NULL);
	_beginthread(worker, 65536, NULL);
	while (GetMessage(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	agt_log("exit");
	return 0;
}
