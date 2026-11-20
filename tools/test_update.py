#!/usr/bin/env python3
"""End-to-end test of signed updates (make test-update; docs/updates.md).

Builds kernels signed with a throwaway key (never the real one):
  A  0.1.0   the built-in kernel, on the EFI partition
  B  0.1.1   a good update
  C  0.1.2   an update that resets before it finishes starting
and serves them from tools/update_server.py, a stand-in for GitHub's
release API over HTTPS (TLS 1.3), with a bad-signature release, a release
whose image does not match its manifest, and a "private" release that
needs a token. Then boots a UEFI disk in QEMU (edk2, NV variables in a
pflash file) and checks that:

  1. bad signatures and bad images are rejected, a private repository gets
     a clear message, a token works, a plain-HTTP mirror works;
  2. `update install` installs B, and the next boot runs B (confirmed);
  3. C is tried UPDATE_MAX_TRIES times, then the loader falls back to A;
     `update rollback` brings B back and B boots again;
  4. a /boot/kernel tampered with on disk is refused by the loader.

Needs clang/lld (make), qemu-system-aarch64 with edk2, openssl, and a
Python whose ssl module has TLS 1.3 (found automatically).
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = os.path.join("build", "updtest")
TOOLS = os.path.join(ROOT, "tools")
ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")
PROMPT = re.compile(rb"sonar:\S*\$ $")


def run(*cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, cwd=ROOT, **kw)


def tls13_python():
    for py in (os.environ.get("TLS_PYTHON"), sys.executable, "/opt/homebrew/bin/python3",
               "/usr/local/bin/python3", "python3"):
        if not py:
            continue
        try:
            r = subprocess.run([py, "-c", "import ssl,sys; sys.exit(0 if ssl.HAS_TLSv1_3 else 1)"],
                               capture_output=True)
            if r.returncode == 0:
                return py
        except OSError:
            pass
    sys.exit("no Python with TLS 1.3 found (set TLS_PYTHON)")


def edk2():
    for d in ("/opt/homebrew/share/qemu", "/usr/local/share/qemu", "/usr/share/qemu"):
        code = os.path.join(d, "edk2-aarch64-code.fd")
        vars_ = os.path.join(d, "edk2-arm-vars.fd")
        if os.path.exists(code) and os.path.exists(vars_):
            return code, vars_
    if os.path.exists("/usr/share/AAVMF/AAVMF_CODE.fd"):
        return "/usr/share/AAVMF/AAVMF_CODE.fd", "/usr/share/AAVMF/AAVMF_VARS.fd"
    sys.exit("edk2 firmware for aarch64 not found")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def accel():
    if sys.platform == "darwin" and os.uname().machine == "arm64":
        return ["-accel", "hvf", "-cpu", "host"]
    return ["-accel", "tcg,thread=multi", "-cpu", "cortex-a76"]


class Boot:
    """One QEMU run, driven over the serial console."""

    def __init__(self, name, code, vars_fd, esp, timeout=240):
        self.name = name
        self.out = bytearray()
        self.timeout = timeout
        cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=3,acpi=off", *accel(),
               "-smp", "2", "-m", "1G", "-global", "virtio-mmio.force-legacy=false",
               "-drive", "if=pflash,format=raw,readonly=on,file=" + code,
               "-drive", "if=pflash,format=raw,file=" + vars_fd,
               "-drive", "file=%s,format=raw,if=none,id=esp" % esp,
               "-device", "virtio-blk-device,drive=esp",
               "-netdev", "user,id=net0", "-device", "virtio-net-device,netdev=net0",
               "-device", "virtio-rng-device", "-nographic"]
        print("\n==== boot: %s ====" % name, flush=True)
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, cwd=ROOT)
        os.set_blocking(self.p.stdout.fileno(), False)
        self.start = time.time()
        self.mark = 0

    def pump(self, wait=0.2):
        import select
        r, _, _ = select.select([self.p.stdout], [], [], wait)
        if r:
            data = self.p.stdout.read() or b""
            if not data and self.p.poll() is not None:
                return False
            self.out.extend(data)
            sys.stdout.buffer.write(data)
            sys.stdout.flush()
        return self.p.poll() is None

    def text(self, since=0):
        return ANSI.sub(b"", bytes(self.out[since:])).decode("utf-8", "replace")

    def wait_prompt(self):
        """Wait for the shell prompt, answering the first-boot setup."""
        deadline = time.time() + self.timeout
        answered = 0
        while time.time() < deadline:
            clean = ANSI.sub(b"", bytes(self.out[self.mark:]))
            if clean.count(b"Press Enter") > answered:
                answered = clean.count(b"Press Enter")
                self.p.stdin.write(b"\r")
                self.p.stdin.flush()
            if PROMPT.search(clean):
                self.mark = len(self.out)
                return
            if not self.pump():
                raise SystemExit("%s: QEMU exited early" % self.name)
        raise SystemExit("%s: no shell prompt" % self.name)

    def cmd(self, line):
        self.wait_prompt()
        self.p.stdin.write(line.encode() + b"\r")
        self.p.stdin.flush()
        self.mark = len(self.out)
        time.sleep(0.05)

    def finish(self, last="poweroff"):
        self.cmd(last)
        deadline = time.time() + 60
        while time.time() < deadline and self.pump():
            pass
        if self.p.poll() is None:
            self.p.kill()
        return self.text()

    def wait_exit(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline and self.pump():
            pass
        if self.p.poll() is None:
            self.p.kill()
        return self.text()


failures = []


def expect(log, what, needle, present=True):
    ok = (needle in log) == present
    print("%s %s" % ("ok  " if ok else "FAIL", what), flush=True)
    if not ok:
        failures.append(what)


def main():
    os.chdir(ROOT)
    shutil.rmtree(T, ignore_errors=True)
    os.makedirs(T)
    py = tls13_python()
    sign = [sys.executable, os.path.join(TOOLS, "sonar_update.py")]

    # Throwaway keys: the test key the kernels trust, and an impostor.
    run(*sign, "keygen", T + "/test.key")
    run(*sign, "keygen", T + "/other.key")
    with open(T + "/test_key.h", "w") as f:
        subprocess.run(sign + ["header", T + "/test.key"], check=True, stdout=f)

    key = "UPDATE_KEY=" + T + "/test_key.h"
    jobs = "-j%d" % (os.cpu_count() or 4)
    for name, ver, extra in (("a", "0.1.0", []), ("b", "0.1.1", []),
                             ("c", "0.1.2", ["TEST_FAIL_BOOT=1"])):
        run("make", jobs, "BUILD=%s/%s" % (T, name), "VERSION=" + ver, key, *extra,
            T + "/%s/Image" % name)

    # Releases.
    rel = T + "/releases/test"

    def release(repo, image, version, keyfile, tamper_image=False, token=None):
        d = os.path.join(rel, repo)
        os.makedirs(d)
        name = "sonar-%s-Image" % version
        shutil.copy(image, os.path.join(d, name))
        with open(os.path.join(d, "sonar-update.txt"), "wb") as f:
            subprocess.run(sign + ["manifest", os.path.join(d, name), version, "--key", keyfile],
                           check=True, stdout=f)
        if tamper_image:
            with open(os.path.join(d, name), "r+b") as f:
                f.seek(8192)
                b = f.read(1)
                f.seek(8192)
                f.write(bytes([b[0] ^ 1]))
        with open(os.path.join(d, "tag"), "w") as f:
            f.write("v" + version)
        if token:
            with open(os.path.join(d, "token"), "w") as f:
                f.write(token)

    release("good", T + "/b/Image", "0.1.1", T + "/test.key")
    release("badsig", T + "/b/Image", "0.1.1", T + "/other.key")
    release("badimg", T + "/b/Image", "0.1.1", T + "/test.key", tamper_image=True)
    release("private", T + "/b/Image", "0.1.1", T + "/test.key", token="test-token-123")
    release("failing", T + "/c/Image", "0.1.2", T + "/test.key")

    # Servers: HTTPS (the GitHub stand-in) and plain HTTP (a mirror).
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", T + "/tls.key",
        "-out", T + "/tls.crt", "-days", "2", "-subj", "/CN=update-test",
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    tport, hport = free_port(), free_port()
    logf = open(T + "/server.log", "w")
    servers = [
        subprocess.Popen([py, os.path.join(TOOLS, "update_server.py"), T + "/releases", "--port",
                          str(tport), "--tls", T + "/tls.crt", T + "/tls.key"],
                         stdout=logf, stderr=logf, cwd=ROOT),
        subprocess.Popen([py, os.path.join(TOOLS, "update_server.py"), T + "/releases", "--port",
                          str(hport)], stdout=logf, stderr=logf, cwd=ROOT),
    ]
    time.sleep(1.5)
    api = "https://10.0.2.2:%d" % tport
    mirror = "http://10.0.2.2:%d/mirror/test/good" % hport

    # The disk: EFI partition with A, SonarFS system partition.
    run(sys.executable, os.path.join(TOOLS, "sonarfs.py"), "mkimage", T + "/system.img", "64M",
        "--label", "SYSTEM")
    esp = T + "/esp.img"
    run(sys.executable, os.path.join(TOOLS, "mkesp.py"), esp, "--system", T + "/system.img",
        "EFI/BOOT/BOOTAA64.EFI=" + T + "/a/Image")
    code, vars_src = edk2()
    vars_fd = T + "/vars.fd"
    shutil.copy(vars_src, vars_fd)

    try:
        # ---- 1: negative cases, then install B
        b = Boot("built-in 0.1.0: checks and install", code, vars_fd, esp)
        b.cmd("dhcp")
        b.cmd("config update.api " + api)
        b.cmd("config update.repo test/badsig")
        b.cmd("update check")
        b.cmd("config update.repo test/badimg")
        b.cmd("update install")
        b.cmd("config update.repo test/private")
        b.cmd("update check")
        b.cmd("config update.token test-token-123")
        b.cmd("update check")
        b.cmd("config update.token none")
        b.cmd("config update.mirror " + mirror)
        b.cmd("update check")
        b.cmd("config update.mirror none")
        b.cmd("config update.repo test/good")
        b.cmd("update check")
        b.cmd("update install")
        b.cmd("update status")
        log = b.finish()
        expect(log, "bad signature rejected", "BAD SIGNATURE")
        expect(log, "tampered image rejected", "does not match the manifest's SHA-256")
        expect(log, "private repository explained", "If github.com/test/private is private")
        expect(log, "token works", "Sonar 0.1.1 is available (this is 0.1.0), release v0.1.1")
        expect(log, "mirror works", "update: checking http://10.0.2.2")
        expect(log, "installed", "installed Sonar 0.1.1 as /boot/kernel")
        expect(log, "status shows it", "/boot/kernel       Sonar 0.1.1")
        expect(log, "no token sent to the download host", "Authorization header sent", False)

        # ---- 2: B boots
        b = Boot("update 0.1.1", code, vars_fd, esp)
        b.cmd("update status")
        b.cmd("update check")
        b.cmd("config update.repo test/failing")
        b.cmd("update install")
        b.cmd("reboot")  # no -no-reboot: QEMU restarts, keeping the UEFI variables
        log = b.text()
        expect(log, "loader starts the update", "update: starting Sonar 0.1.1 from /boot/kernel")
        expect(log, "update running", "running:  Sonar 0.1.1, installed update")
        expect(log, "start confirmed", "marked as good")
        expect(log, "up to date", "Sonar 0.1.1 is up to date")
        expect(log, "failing update installed", "installed Sonar 0.1.2")

        # ---- 3: C fails to start three times, A takes over; roll back to B
        print("\n==== boot: failing 0.1.2, three tries, then the built-in kernel ====", flush=True)
        b.out = bytearray()
        b.mark = 0
        b.timeout = 400
        b.cmd("update status")
        b.cmd("update rollback")
        log = b.finish()
        expect(log, "failing update was tried", "update: test build: resetting")
        expect(log, "fallback after 3 tries", "did not finish starting 3 times")
        expect(log, "fallback runs the built-in kernel", "running:  Sonar 0.1.0, built-in kernel")
        expect(log, "rollback", "swapped /boot/kernel and /boot/kernel.prev")

        b = Boot("after rollback", code, vars_fd, esp)
        b.cmd("update status")
        log = b.finish()
        expect(log, "rolled-back update starts", "starting Sonar 0.1.1 from /boot/kernel (signature verified)")
        expect(log, "without a trial (it was confirmed)", "trial start", False)
        expect(log, "it is 0.1.1", "running:  Sonar 0.1.1, installed update")

        # ---- 4: tamper with /boot/kernel on the disk
        run(sys.executable, os.path.join(TOOLS, "sonarfs.py"), "get", esp, "/boot/kernel",
            T + "/kernel.bundle")
        data = bytearray(open(T + "/kernel.bundle", "rb").read())
        data[len(data) // 2] ^= 0x40
        open(T + "/kernel.bundle", "wb").write(data)
        run(sys.executable, os.path.join(TOOLS, "sonarfs.py"), "put", esp, T + "/kernel.bundle",
            "/boot/kernel")
        b = Boot("tampered /boot/kernel", code, vars_fd, esp)
        b.cmd("update status")
        b.cmd("update verify /boot/kernel")
        log = b.finish()
        expect(log, "loader refuses the tampered kernel",
               "ignoring /boot/kernel: the kernel image does not match")
        expect(log, "built-in kernel runs", "running:  Sonar 0.1.0, built-in kernel")
        expect(log, "verify command rejects it", "/boot/kernel: REJECTED")
        run(sys.executable, os.path.join(TOOLS, "sonarfs.py"), "fsck", esp)
    finally:
        for s in servers:
            s.terminate()

    print("\ntest-update: %d failure(s)%s" % (len(failures), ": " + ", ".join(failures) if failures else ""))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
