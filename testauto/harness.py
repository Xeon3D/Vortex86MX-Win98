"""PC-side test harness for the eBOX-3300MX (drives EBOXAGT.EXE on the box).

The agent polls exchange/out/JOB/JOB.TXT over FTP and reports into exchange/in:
  in/HB.TXT              heartbeat (boot number, uptime, last job seq, state)
  in/RUN/<seq>/          RESULT.TXT (rewritten after each step), OUT<n>.TXT, STAT.TXT, *.BMP

Usage:
  python harness.py status
  python harness.py job "RUN SEND.BAT" "STAT"           ad-hoc job, waits for the result
  python harness.py run install --v V33R --n N10        install builds, restart, verify
  python harness.py run boot --boots 5                  5 restarts, timed, CMOS halt check
  python harness.py run net --nets 3                    BIGTEST round trips (md5 + speed)
  python harness.py run install boot net --v V33R       suites run in that order
Reports: exchange/reports/<time>-<suites>/report.md + report.json. Exit code 1 if a check FAILs.
"""
import argparse
import datetime as dt
import hashlib
import json
import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# bootlog entries that every boot of this box has (not caused by our drivers)
KNOWN_BOOT_FAILS = {"ndis2sup.vxd", "MTRR", "VPD", "SDVXD"}
BIGTEST_MD5 = "42597f988195c28bd63d63377c449017"


class Harness:
    def __init__(self, root, box, hb_timeout, boot_timeout):
        self.root = root
        self.box = box
        self.hb_timeout = hb_timeout
        self.boot_timeout = boot_timeout
        self.checks = []
        self.log_lines = []

    # ---------------------------------------------------------------- paths / io
    def p(self, *parts):
        return os.path.join(self.root, *parts)

    def say(self, msg):
        line = "%s %s" % (dt.datetime.now().strftime("%H:%M:%S"), msg)
        print(line, flush=True)
        self.log_lines.append(line)

    def check(self, name, status, detail=""):
        self.checks.append({"check": name, "status": status, "detail": detail})
        self.say("[%s] %s %s" % (status, name, detail))

    @staticmethod
    def read(path, binary=False):
        try:
            with open(path, "rb" if binary else "r", errors=None if binary else "replace") as f:
                return f.read()
        except OSError:
            return None

    @staticmethod
    def md5(path):
        h = hashlib.md5()
        try:
            with open(path, "rb") as f:
                for b in iter(lambda: f.read(1 << 20), b""):
                    h.update(b)
        except OSError:
            return None
        return h.hexdigest()

    # ---------------------------------------------------------------- heartbeat
    def hb(self):
        path = self.p("in", "HB.TXT")
        text = self.read(path)
        if text is None:
            return None
        d = dict(l.split("=", 1) for l in text.splitlines() if "=" in l)
        d["age"] = time.time() - os.path.getmtime(path)
        for k in ("boot", "lastseq", "uptime"):
            try:
                d[k] = int(d.get(k, 0))
            except ValueError:
                d[k] = 0
        return d

    def need_agent(self):
        h = self.hb()
        if h is None or h["age"] > self.hb_timeout:
            raise SystemExit("agent not alive (last heartbeat %s)" %
                             ("never" if h is None else "%d s ago" % h["age"]))
        return h

    def ping(self):
        r = subprocess.run(["ping", "-n", "1", "-w", "700", self.box],
                           capture_output=True, text=True, errors="replace")
        return "TTL=" in r.stdout

    # ---------------------------------------------------------------- jobs
    def next_seq(self):
        state = self.p("reports", "seq.txt")
        local = int((self.read(state) or "0").strip() or 0)
        h = self.hb()
        seq = max(local, h["lastseq"] if h else 0) + 1
        os.makedirs(self.p("reports"), exist_ok=True)
        with open(state, "w") as f:
            f.write("%d\n" % seq)
        return seq

    def post(self, lines):
        """Write a job; returns its seq. The agent sees a change by MDTM (1 s) + SIZE."""
        seq = self.next_seq()
        os.makedirs(self.p("out", "JOB"), exist_ok=True)
        job = self.p("out", "JOB", "JOB.TXT")
        old = int(os.path.getmtime(job)) if os.path.exists(job) else None
        if old is not None and int(time.time()) <= old:
            time.sleep(1.1)
        body = "SEQ %d\n# posted %s\n%s\n" % (seq, dt.datetime.now().isoformat(), "\n".join(lines))
        tmp = job + ".tmp"
        with open(tmp, "w", newline="\r\n") as f:
            f.write(body)
        os.replace(tmp, job)
        self.say("job %d posted: %s" % (seq, " | ".join(lines)))
        return seq

    def result(self, seq):
        text = self.read(self.p("in", "RUN", str(seq), "RESULT.TXT"))
        if text is None:
            return None
        steps, end = [], None
        for l in text.splitlines():
            if l.startswith("STEP "):
                steps.append({"cmd": l.split(" ", 2)[2], "rc": None, "ms": None, "notes": []})
            elif l.strip().startswith("rc=") and steps:
                m = re.search(r"rc=(-?\d+) t=(\d+)", l)
                if m:
                    steps[-1]["rc"], steps[-1]["ms"] = int(m.group(1)), int(m.group(2))
            elif l.startswith("  ") and steps:
                steps[-1]["notes"].append(l.strip())
            elif l.startswith("END "):
                end = l.split()[1]
        return {"steps": steps, "end": end, "text": text}

    def wait_job(self, seq, timeout):
        """Wait until RESULT.TXT has its END line."""
        t0 = time.time()
        picked = False
        while time.time() - t0 < timeout:
            r = self.result(seq)
            if r is not None:
                if not picked:
                    self.say("job %d picked up after %.0f s" % (seq, time.time() - t0))
                    picked = True
                if r["end"]:
                    return r
            elif time.time() - t0 > max(30, 3 * 5 + 10) and not picked:
                h = self.hb()
                if h is None or h["age"] > self.hb_timeout:
                    self.say("job %d: agent silent" % seq)
            time.sleep(1)
        return self.result(seq)

    def job(self, lines, timeout=900):
        seq = self.post(lines)
        r = self.wait_job(seq, timeout)
        return seq, r

    def wait_reboot(self, old_boot, end_time):
        """After an END restart: track ping down/up and the first heartbeat of the new boot."""
        t = {"down": None, "up": None, "hb": None}
        stop = threading.Event()

        def pinger():
            seen_down = False
            while not stop.is_set():
                ok = self.ping()
                now = time.time()
                if not ok and not seen_down:
                    seen_down, t["down"] = True, now
                elif ok and seen_down and t["up"] is None:
                    t["up"] = now
                stop.wait(1)

        th = threading.Thread(target=pinger, daemon=True)
        th.start()
        try:
            while time.time() - end_time < self.boot_timeout:
                h = self.hb()
                if h and h["boot"] != old_boot and h["age"] < 30:
                    t["hb"] = time.time() - h["age"]
                    break
                time.sleep(2)
        finally:
            stop.set()
            th.join(3)
        rel = {k: (round(v - end_time, 1) if v else None) for k, v in t.items()}
        return rel

    def restart_job(self, lines, label):
        """Run a job that ends in RESTART; returns (seq, result, timings) or raises on hang."""
        h = self.need_agent()
        seq, r = self.job(lines + ["RESTART"])
        if r is None or r["end"] != "restart":
            self.check(label + " job", "FAIL", "job %d did not reach RESTART: %s" %
                       (seq, None if r is None else r["end"]))
            return seq, r, None
        end_time = os.path.getmtime(self.p("in", "RUN", str(seq), "RESULT.TXT"))
        tm = self.wait_reboot(h["boot"], end_time)
        if tm["hb"] is None:
            self.check(label, "FAIL", "no heartbeat %d s after RESTART (hang? ping down=%s up=%s)"
                       % (self.boot_timeout, tm["down"], tm["up"]))
            raise HangError(label)
        fmt = lambda v: "-" if v is None else "%.0f s" % v
        self.check(label, "PASS", "ping lost %s, ping back %s, agent back %s after RESTART" %
                   (fmt(tm["down"]), fmt(tm["up"]), fmt(tm["hb"])))
        return seq, r, tm

    # ---------------------------------------------------------------- parsing
    def stat(self, seq):
        text = self.read(self.p("in", "RUN", str(seq), "STAT.TXT")) or ""
        files, texts, cur = {}, {}, None
        for l in text.splitlines():
            if l.startswith("file "):
                parts = l.split()
                files[parts[1].upper()] = None if parts[2] == "missing" else (int(parts[2]), parts[3])
            elif l.startswith("text "):
                cur = l[5:].upper()
                texts[cur] = []
            elif l.startswith("| ") and cur:
                texts[cur].append(l[2:])
        return files, texts

    def boot_fails(self):
        text = self.read(self.p("in", "bootlog.txt")) or ""
        bad = set()
        for l in text.splitlines():
            m = re.search(r"(LoadFailed|DEVICEINITFAILED|INITCOMPLETEFAILED)\s*=\s*(\S+)", l, re.I)
            if m:
                bad.add(os.path.basename(m.group(2)))
        return bad

    def ftplog_since(self, t0, pattern):
        """seconds= values of transfers matching pattern logged at or after t0."""
        text = self.read(self.p("ftplog.txt")) or ""
        out = []
        for l in text.splitlines():
            m = re.match(r"\[I (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\]", l)
            if not m or not re.search(pattern, l):
                continue
            when = dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S").timestamp()
            s = re.search(r"seconds=([\d.]+)", l)
            if when >= t0 - 1 and s:
                out.append(float(s.group(1)))
        return out

    # ---------------------------------------------------------------- suites
    def suite_stat(self, a):
        seq, r = self.job(["STAT"], 120)
        files, texts = self.stat(seq)
        for f, v in files.items():
            self.say("  %s %s" % (f, v))
        self.check("stat", "PASS" if r and r["end"] == "ok" else "FAIL", "job %d" % seq)

    def suite_install(self, a):
        lines = []
        if a.v:
            lines.append("RUN TRY.BAT %s" % a.v)
        if a.n:
            lines.append("RUN TRY.BAT %s" % a.n)
        if not lines:
            self.check("install", "FAIL", "give --v and/or --n")
            return
        for bid in filter(None, (a.v, a.n)):
            if not os.path.isdir(self.p("out", bid)):
                self.check("install %s" % bid, "FAIL", "exchange/out/%s does not exist" % bid)
                return
        seq, r, tm = self.restart_job(lines + ["STAT"], "install restart")
        if r:
            files, texts = self.stat(seq)
            ini = texts.get("C:\\WINDOWS\\WININIT.INI", [])
            self.check("install queued", "PASS" if any("PEND" in l.upper() for l in ini) else "FAIL",
                       "WININIT.INI: %d lines" % len(ini))
        if tm is None:
            return
        self.verify_installed(a)

    def verify_installed(self, a):
        t0 = time.time()
        seq, r = self.job(["STAT", "RUN SEND.BAT", "RUN NETSEND.BAT"], 300)
        files, texts = self.stat(seq)
        want = []
        if a.v:
            want += [("VESAMINI.DRV", a.v), ("VESAMINI.VXD", a.v)]
        if a.n:
            want += [("RDCPCI5.SYS", a.n)]
        for name, bid in want:
            src = self.p("out", bid, name)
            got = files.get("C:\\WINDOWS\\SYSTEM\\" + name)
            exp = self.md5(src)
            ok = got is not None and got[1] == exp
            self.check("installed %s = %s" % (name, bid), "PASS" if ok else "FAIL",
                       "box %s, out %s" % (got[1][:8] if got else "missing", (exp or "missing")[:8]))
        for key, bid in (("C:\\M2010\\B\\CURV.TXT", a.v), ("C:\\M2010\\B\\CURN.TXT", a.n)):
            if bid:
                cur = " ".join(texts.get(key, [])).strip()
                self.check("%s = %s" % (os.path.basename(key), bid),
                           "PASS" if cur.upper() == bid.upper() else "FAIL", repr(cur))
        if "C:\\M2010\\ROLLBACK.TXT" in texts:
            self.check("no rollback", "FAIL", " ".join(texts["C:\\M2010\\ROLLBACK.TXT"]))
        self.check_bootlog(t0)

    def fresh(self, name, t0):
        path = self.p("in", name)
        return os.path.exists(path) and os.path.getmtime(path) >= t0 - 1

    def check_bootlog(self, t0):
        if not self.fresh("bootlog.txt", t0):
            self.check("bootlog", "WARN", "bootlog.txt not uploaded by SEND (BootLog off?)")
            return
        new = self.boot_fails() - KNOWN_BOOT_FAILS
        self.check("bootlog", "PASS" if not new else "FAIL",
                   "new failures: %s" % ", ".join(sorted(new)) if new else "only the known 4")

    def suite_boot(self, a):
        times = []
        for i in range(a.boots):
            seq, r, tm = self.restart_job(["STAT"], "restart %d/%d" % (i + 1, a.boots))
            if tm is None:
                return
            times.append(tm["hb"])
        t0 = time.time()
        seq, r = self.job(["RUN NETSEND.BAT", "RUN SEND.BAT"], 300)
        if not self.fresh("r6040dbg.txt", t0):
            self.check("R6040 halt clean x%d" % a.boots, "FAIL", "r6040dbg.txt not uploaded by NETSEND")
            return
        recs = [l for l in (self.read(self.p("in", "r6040dbg.txt")) or "").splitlines() if " cp=" in l]
        last = recs[-a.boots:]
        bad = [l for l in last if " cp=4E " not in l]
        self.check("R6040 halt clean x%d" % a.boots, "PASS" if len(last) == a.boots and not bad else "FAIL",
                   "; ".join(l[:40] for l in bad) if bad else "last %d CMOS records cp=4E" % len(last))
        self.check_bootlog(t0)
        if times:
            self.check("boot time", "INFO", "agent back after %s s (min %.0f max %.0f)" %
                       (times, min(times), max(times)))

    def suite_net(self, a):
        back = self.p("in", "BIGBACK.BIN")
        for i in range(a.nets):
            if os.path.exists(back):
                os.remove(back)
            t0 = time.time()
            seq, r = self.job(["RUN /T=600 BIGTEST.BAT"], 900)
            m = self.md5(back)
            down = self.ftplog_since(t0, r"RETR .*BIGTEST\.BIN")
            up = self.ftplog_since(t0, r"STOR .*BIGBACK\.BIN")
            self.check("bigtest %d md5" % (i + 1), "PASS" if m == BIGTEST_MD5 else "FAIL",
                       "md5 %s" % (m or "no BIGBACK.BIN"))
            if down and up:
                slow = i > 0 and up[-1] > a.max_up
                self.check("bigtest %d speed" % (i + 1), "WARN" if slow else "INFO",
                           "down %.1f s, up %.1f s (32 MB)" % (down[-1], up[-1]))
        if os.path.exists(back):
            os.remove(back)

    # ---------------------------------------------------------------- report
    def report(self, suites, args, t_start):
        stamp = dt.datetime.fromtimestamp(t_start).strftime("%Y%m%d-%H%M%S")
        d = self.p("reports", "%s-%s" % (stamp, "-".join(suites)))
        os.makedirs(d, exist_ok=True)
        fails = [c for c in self.checks if c["status"] == "FAIL"]
        h = self.hb() or {}
        with open(os.path.join(d, "report.json"), "w") as f:
            json.dump({"suites": suites, "args": vars(args), "start": stamp,
                       "duration_s": round(time.time() - t_start), "checks": self.checks}, f, indent=1)
        with open(os.path.join(d, "report.md"), "w") as f:
            f.write("# eBOX test run %s: %s\n\n" % (stamp, " + ".join(suites)))
            f.write("Result: **%s** (%d checks, %d failed), %d s. V=%s N=%s, agent %s, boot %s.\n\n" %
                    ("FAIL" if fails else "PASS", len(self.checks), len(fails),
                     time.time() - t_start, args.v, args.n, h.get("agent"), h.get("boot")))
            f.write("| Status | Check | Detail |\n| --- | --- | --- |\n")
            for c in self.checks:
                f.write("| %s | %s | %s |\n" % (c["status"], c["check"], c["detail"].replace("|", "/")))
            f.write("\n## Log\n\n```\n%s\n```\n" % "\n".join(self.log_lines))
        self.say("report: %s" % os.path.join(d, "report.md"))
        return 1 if fails else 0


class HangError(Exception):
    pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=os.path.join(HERE, "..", "exchange"), help="exchange folder")
    ap.add_argument("--box", default="10.0.0.108")
    ap.add_argument("--hb-timeout", type=int, default=150, help="heartbeat older than this = dead")
    ap.add_argument("--boot-timeout", type=int, default=300, help="restart to heartbeat limit")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    j = sub.add_parser("job")
    j.add_argument("lines", nargs="+")
    j.add_argument("--timeout", type=int, default=900)
    r = sub.add_parser("run")
    r.add_argument("suites", nargs="+", choices=["stat", "install", "boot", "net"])
    r.add_argument("--v", help="display build ID (exchange/out/V..)")
    r.add_argument("--n", help="network build ID (exchange/out/N..)")
    r.add_argument("--boots", type=int, default=5)
    r.add_argument("--nets", type=int, default=3)
    r.add_argument("--max-up", type=float, default=8.0, help="WARN if a later upload takes longer (s)")
    a = ap.parse_args()
    H = Harness(os.path.abspath(a.root), a.box, a.hb_timeout, a.boot_timeout)

    if a.cmd == "status":
        h = H.hb()
        if h is None:
            print("no heartbeat yet (in/HB.TXT missing)")
            return 1
        for k, v in h.items():
            print("%-13s %s" % (k, round(v) if k == "age" else v))
        print("ping         ", H.ping())
        return 0 if h["age"] <= a.hb_timeout else 1
    if a.cmd == "job":
        H.need_agent()
        seq, res = H.job(a.lines, a.timeout)
        print(res["text"] if res else "no result for job %d" % seq)
        return 0 if res and res["end"] else 1

    t_start = time.time()
    H.need_agent()
    try:
        for s in a.suites:
            H.say("== suite %s" % s)
            getattr(H, "suite_" + s)(a)
    except HangError:
        H.say("stopped: the box did not come back; power-cycle it, then rerun")
    except KeyboardInterrupt:
        H.check("interrupted", "FAIL", "Ctrl+C")
    return H.report(a.suites, a, t_start)


if __name__ == "__main__":
    sys.exit(main())
