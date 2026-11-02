#!/usr/bin/env python3
"""Headless boot test for NiteKernel.

Boots the ISO in QEMU, waits for the shell prompt, then drives the serial
console exactly like a terminal would and checks what comes back. Exits
non-zero on the first failed expectation so it can gate a change.

    python3 tools/boottest.py                 # build/nitekernel.iso
    python3 tools/boottest.py --apps          # also exercise the loaders
    python3 tools/boottest.py --net user      # DHCP + driver acquisition

The log of the last run is written to build/boottest.log.
"""
import argparse
import hashlib
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
BOOT_MARKER = "Type 'help' for the command list."


class Boot:
    """A QEMU instance talked to over the serial line."""

    def __init__(self, args):
        qemu = shutil.which(args.qemu)
        if not qemu:
            print("error: %s not found; install qemu-system-x86" % args.qemu,
                  file=sys.stderr)
            raise SystemExit(2)

        cmd = [
            qemu,
            "-cdrom", args.iso,
            "-m", "512M",
            "-display", "none",
            "-no-reboot",
            "-no-shutdown",
            "-accel", args.accel,
            "-serial", "stdio",
        ]
        if args.apps:
            cmd += ["-drive", "file=%s,format=raw,if=ide" % args.apps]
        if args.net == "none":
            cmd += ["-nic", "none"]
        else:
            cmd += ["-netdev", "user,id=n0", "-device", "e1000,netdev=n0"]
        # Extra arguments (e.g. -L for a QEMU installed outside the usual
        # prefix) can come from the command line or from $QEMU_ARGS.
        cmd += args.qemu_arg + os.environ.get("QEMU_ARGS", "").split()

        self.errfile = open(os.path.join(BUILD, "boottest.log"), "wb")
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.errfile, bufsize=0)
        self.buf = b""

    @property
    def text(self):
        return self.buf.decode("utf-8", "replace")

    def pump(self, timeout):
        """Collect whatever is available for up to `timeout` seconds."""
        fd = self.proc.stdout.fileno()
        ready, _, _ = select.select([fd], [], [], timeout)
        if not ready:
            return
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            return
        if chunk:
            self.buf += chunk

    def wait_for(self, needle, timeout, start=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle(self.text[start:]) if callable(needle) else needle in self.text[start:]:
                return True
            if self.proc.poll() is not None:
                self.pump(0.1)
                return False
            self.pump(0.2)
        # one last look after the deadline
        self.pump(0.2)
        return needle(self.text[start:]) if callable(needle) else needle in self.text[start:]

    def send(self, line):
        self.proc.stdin.write((line + "\r").encode())
        self.proc.stdin.flush()

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.errfile.close()


def strip_ansi(s):
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", s)


def start_mirror(directory, port=8080):
    """Serve the app store so the guest can `lash install` from it."""
    if not os.path.isdir(directory):
        raise SystemExit("no such app store directory: %s" % directory)
    proc = subprocess.Popen(
        [sys.executable, os.path.join(ROOT, "tools", "mirror_server.py"),
         directory, str(port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                print("OK    app mirror on http://127.0.0.1:%d" % port)
                return proc
        except OSError:
            time.sleep(0.1)
    proc.terminate()
    raise SystemExit("the app mirror did not come up on port %d" % port)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", default=os.path.join(BUILD, "nitekernel.iso"))
    ap.add_argument("--apps", default=None,
                    help="NEOFS test image (from tools/build_testapps.py)")
    ap.add_argument("--appstore", default=None,
                    help="app store directory to serve over HTTP as the mirror")
    ap.add_argument("--net", choices=["none", "user"], default="none",
                    help="none is offline and fast; user exercises DHCP + "
                         "driver acquisition")
    ap.add_argument("--accel", default="tcg")
    ap.add_argument("--qemu", default="qemu-system-x86_64")
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="seconds to wait for the first prompt")
    ap.add_argument("--qemu-arg", action="append", default=[], metavar="ARG",
                    help="extra QEMU argument, repeatable (also QEMU_ARGS env)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.iso):
        print("error: %s is missing; run make first" % args.iso, file=sys.stderr)
        return 2
    os.makedirs(BUILD, exist_ok=True)

    if args.apps:
        if not os.path.exists(args.apps):
            print("error: %s is missing; run python3 tools/build_testapps.py"
                  % args.apps, file=sys.stderr)
            return 2
        # Boot a copy: the guest writes to the volume (files, driver store), and
        # the packed image must not drift from one run to the next.
        fresh = os.path.join(BUILD, "boottest-apps.img")
        shutil.copyfile(args.apps, fresh)
        args.apps = fresh

    # `lash install` needs both a volume to install into and a reachable
    # mirror, so an app store test always runs with user-mode networking.
    mirror = None
    if args.appstore:
        args.net = "user"
        mirror = start_mirror(args.appstore, 8080)

    boot = Boot(args)
    failures = []

    def fail(msg):
        failures.append(msg)
        print("FAIL  " + msg)

    def check(desc, needle, timeout=30):
        """Run a command and require `needle` in the output it produces."""
        start = len(boot.buf)
        print("  ...  %s" % desc, end="", flush=True)
        if not boot.wait_for(needle, timeout, start):
            print("\rFAIL  %s" % desc)
            fail("%s: expected %r" %
                 (desc, needle if not callable(needle) else "<predicate>"))
            return False
        print("\rOK    %s" % desc)
        return True

    def command(line, needle, desc=None, timeout=30):
        desc = desc or line
        start = len(boot.buf)
        print("  ...  %s" % desc, end="", flush=True)
        boot.send(line)
        if not boot.wait_for(needle, timeout, start):
            print("\rFAIL  %s" % desc)
            fail("%s: no match for %r after sending %r" %
                 (desc, needle if not callable(needle) else "<predicate>", line))
            return False
        print("\rOK    %s" % desc)
        return True

    try:
        # --- boot ---
        if not boot.wait_for(BOOT_MARKER, args.timeout):
            fail("kernel did not reach the shell prompt within %.0fs" % args.timeout)
            return 1
        print("OK    booted to the shell prompt")

        for marker in ("[ OK ] Multiboot2 handoff verified",
                       "IDT installed",
                       "PMM: alloc/free cycle ok",
                       "VMM: mapped",
                       "HEAP: kmalloc",
                       "syscall gate int 0x80 installed"):
            if marker not in boot.text:
                fail("boot log is missing %r" % marker)
        print("OK    boot self-tests present in the log")

        # --- shell basics ---
        command("help", "recall list of this session's commands",
                "help lists the command table")
        command("uptime", "uptime: ", "uptime reports the PIT tick count")
        command("mem", "Physical:", "mem reports the allocators")
        command("echo boottest-ok", "\nboottest-ok\n", "echo prints its argument")

        # --- lash: pipes and redirection (builtin output must be captured) ---
        command("lash -c 'echo pipe-line | cat'", "\npipe-line\n",
                "pipeline through the fd layer")

        if args.apps:
            # --- NEOFS round trip, verified with an independent digest ---
            command("write bt.txt hello", "wrote 6 bytes to bt.txt",
                    "write a file on NEOFS")
            payload = b"hello\x00"          # write stores the NUL too
            digest = hashlib.sha256(payload).hexdigest()
            command("sha256sum bt.txt", digest,
                    "sha256sum matches the host digest")
            command("cat bt.txt", "\nhello\n", "cat reads the file back")
            command("cat < bt.txt", "\nhello\n",
                    "input redirection feeds cat")
            command("lash -c 'echo redir > /bt2.txt; cat /bt2.txt'", "\nredir\n",
                    "> captures builtin output into a file")
            command("rm bt.txt", "removed bt.txt", "remove a file")
            command("rm /bt2.txt", "removed /bt2.txt", "remove the redirect target")

            # --- history ---
            command("history", lambda text: re.search(r"\n\s+\d+\s+rm bt\.txt", text) is not None,
                    "history lists earlier commands")

            # --- loaders ---
            command("run hello_elf", "hello from ELF64",
                    "ELF64 ET_EXEC loader")
            command("run hello_pe.exe", "hello from a Windows PE32+",
                    "PE32+ loader")
            command("run real_win.exe", "real mingw PE",
                    "real mingw .exe with imports bound")

        # --- app store: format ranking, download, verified install ---
        if args.appstore:
            command("apps hello", "macOS only, never selected",
                    "the catalogue lists the format it refuses to run")
            command("install hello", "picked hello.lash (lash,",
                    "install ranks .lash above .exe and never picks the .dmg")

            # sysinfo is not on the test volume, so this one really downloads.
            with open(os.path.join(args.appstore, "apps", "sysinfo.lash"), "rb") as f:
                digest = hashlib.sha256(f.read()).hexdigest()
            command("install sysinfo", "installed sysinfo.lash",
                    "install downloads a package and stores it")
            command("sha256sum sysinfo.lash", digest,
                    "the stored package matches the mirror byte for byte")
            command("lash sysinfo.lash", "sysinfo: NiteKernel",
                    "the installed package runs")
            command("install sysinfo", "already installed",
                    "installing the same app twice is a no-op")
            command("install winhello",
                    lambda t: ("picked winhello.exe (exe" in t) or
                               ("publishes no package this machine can run" in t),
                    "a Windows-only app installs, or says why it cannot")

        if "*** KERNEL PANIC ***" in boot.text:
            fail("the kernel panicked during the run")

        print()
        if failures:
            print("%d check(s) failed" % len(failures))
            tail = strip_ansi(boot.text)[-3000:]
            print("\n--- last console output ---\n" + tail)
            return 1
        print("all checks passed")
        return 0
    finally:
        logpath = os.path.join(BUILD, "boottest.log.out")
        with open(logpath, "w") as f:
            f.write(strip_ansi(boot.text))
        boot.close()
        if mirror is not None:
            mirror.terminate()
            try:
                mirror.wait(timeout=5)
            except subprocess.TimeoutExpired:
                mirror.kill()


if __name__ == "__main__":
    sys.exit(main())
