# Moving files on and off the stick: it can only listen, and a half-close silently truncates

The stick can never dial out, so both directions need it listening; on the
pull side, a naive `nc` half-close truncates the transfer to a couple of
kilobytes with no error at all.

*Last verified: 2026-09-21*

---

## What

**If the stick runs an SSH server, none of this is necessary.**
`ssh host 'cat > /tmp/x && chmod +x /tmp/x' < file` transfers correctly with
nothing else armed, no half-close truncation, and no second port — verified
by checksum on both ends for a 404 KB binary. The netcat forms below are only
needed for a stick reachable solely over telnet.

The stick **cannot open connections outward**. So it always listens in both
directions, and the host always connects; only the redirection differs:

    push:  (stick) nc -l -p 12345 > /tmp/hello     (host) nc -w 5 IP 12345 < file
    pull:  (stick) nc -l -p 12346 < /bin/busybox   (host) nc -d -w 5 IP 12346 > file

Use different ports so a push and a pull can be armed at the same time.

## Why it matters

**`-d` on the pull side is mandatory.** Without it, netcat forwards the
host's own stdin to the socket, and stdin already at EOF half-closes the
connection — which the sender reads as "stop". Measured on a 40,000-byte
transfer:

| receiver | bytes received |
|---|---|
| `nc -w 3 … < /dev/null` | 2,048 |
| client calling `shutdown(SHUT_WR)` | 1,024 |
| `nc -d -w 3 …` | **40,000** |

**netcat reports nothing on truncation** — no length, no checksum, no error.
You get a plausible file and a binary that fails for reasons you will
misattribute to something else entirely. Print size and a checksum on the
host and check both against the stick, as part of the transfer script rather
than only in documentation.

Two more, both easy to miss:

- **The stick's `nc` never exits on its own.** It has to be interrupted by
  hand. Correspondingly the host side also hangs after EOF, because the peer
  never closes — so the host side needs `-w` as well.
- **The rootfs is read-only.** `/tmp` is where things go, and on the stock
  firmware it turns out to be a symlink into a RAM-backed filesystem, not
  plain tmpfs (see [the rootfs symlinks note](rtl9601-rootfs-symlinks-into-var.md)
  for why that distinction matters). This is also the safety property: a bad
  binary cannot brick the stick and does not survive a reboot.

Base64-over-telnet works but is strictly worse: it re-encodes the payload and
hits the terminal line discipline's roughly 1024-byte line buffer, truncating
just as silently.

A shell trap that can hide the real fix: a `-d` capability probe of the form
`nc -h 2>&1 | grep -q …` always reports "unsupported" under `pipefail`,
because the help/version invocation itself exits non-zero. Capture the output
first, then grep it. General form: under `pipefail`, any feature probe whose
first stage is a help/version command that exits non-zero will always report
"unsupported".

## On a from-source replacement image: real scp, and SSH keys

A from-source replacement image can ship a real `scp` (for example dropbear's
own), so `scp -O file root@stick:/tmp/` and the reverse both work directly.
Note that dropbear's `scp` speaks only the legacy `scp` protocol with no SFTP
subsystem, so an OpenSSH 9.0+ client that defaults to SFTP needs `-O` to fall
back. The stock (OEM) firmware ships no scp server at all, so on that image
the `ssh 'cat > file' < file` form above is what you have.
