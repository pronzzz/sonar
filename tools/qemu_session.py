#!/usr/bin/env python3
"""Drive a Sonar OS session in QEMU over the serial console.

Boots the OS, waits for the shell prompt before sending each command, and
prints the transcript. Used for automated end-to-end tests.

  qemu_session.py [--timeout S] [--cmd-timeout S] [--expect TEXT]... -- QEMU ARGS...
      < commands (one per line)

Lines starting with '#wait REGEX' wait for output instead of sending text;
'#send TEXT' sends text without waiting for a prompt.
Exit status is 1 if an --expect string is missing or the session times out.
"""
import os
import re
import select
import subprocess
import sys
import time

ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")
PROMPT = re.compile(rb"sonar:\S*\$ $")


def main():
    args = sys.argv[1:]
    timeout = 120.0
    cmd_timeout = 60.0
    expects = []
    while args and args[0] != "--":
        a = args.pop(0)
        if a == "--timeout":
            timeout = float(args.pop(0))
        elif a == "--cmd-timeout":
            cmd_timeout = float(args.pop(0))
        elif a == "--expect":
            expects.append(args.pop(0))
        else:
            sys.exit(f"unknown option {a}")
    if not args:
        sys.exit("usage: qemu_session.py [options] -- qemu-system-aarch64 ...")
    qemu = args[1:]
    script = [l.rstrip("\n") for l in sys.stdin.readlines()]

    p = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    out = bytearray()
    start = time.time()
    fd = p.stdout.fileno()

    def pump(deadline):
        r, _, _ = select.select([fd], [], [], max(0.0, min(0.2, deadline - time.time())))
        if r:
            data = os.read(fd, 65536)
            if not data:
                return False
            out.extend(data)
            sys.stdout.buffer.write(data)
            sys.stdout.flush()
        return True

    consumed = [0]

    def wait_for(pattern, limit):
        deadline = min(time.time() + limit, start + timeout)
        while time.time() < deadline:
            clean = ANSI.sub(b"", bytes(out[consumed[0]:]))
            if pattern.search(clean):
                consumed[0] = len(out)
                return True
            if not pump(deadline):
                return False
        return False

    ok = True
    for line in script:
        if line.startswith("#wait "):
            if not wait_for(re.compile(line[6:].encode()), cmd_timeout):
                print(f"\n[qemu_session] timed out waiting for {line[6:]!r}", file=sys.stderr)
                ok = False
                break
            continue
        if line.startswith("#send "):
            p.stdin.write(line[6:].encode() + b"\r")
            p.stdin.flush()
            continue
        if not wait_for(PROMPT, cmd_timeout):
            print(f"\n[qemu_session] timed out waiting for prompt before {line!r}",
                  file=sys.stderr)
            ok = False
            break
        # Type slowly enough for the 16-byte UART FIFO.
        for ch in (line + "\r").encode():
            p.stdin.write(bytes([ch]))
            p.stdin.flush()
            time.sleep(0.002)
    # Collect trailing output briefly (or until QEMU exits).
    deadline = time.time() + 3
    while time.time() < deadline and p.poll() is None:
        if not pump(deadline):
            break
    if p.poll() is None:
        p.kill()
    p.wait()
    text = ANSI.sub(b"", bytes(out)).decode("utf-8", "replace")
    for e in expects:
        if e not in text:
            print(f"\n[qemu_session] MISSING expected output: {e!r}", file=sys.stderr)
            ok = False
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
