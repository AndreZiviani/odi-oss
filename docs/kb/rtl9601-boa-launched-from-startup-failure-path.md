# The stock web server only runs because an earlier boot step failed

On the stock firmware, seeing the web server running is a symptom that an
earlier boot step failed, not a sign that boot succeeded — and it is safe to
remove the web server binary entirely from a custom image.

*Last verified: 2026-09-16*

---

## What

The stock boot script only launches the stock web server (boa) from its
**failure path** — the branch taken when an earlier boot step did not
complete successfully.

**And that is the path taken on real devices.** A running stock stick shows
the web server process alive with the boot script itself no longer running.
So the web server being up says nothing good about the rest of the boot
sequence — it is the residue of a boot step that failed, not evidence that
boot went well.

## Why it matters

Two consequences:

- **Removing the web server binary from a custom image is safe.** Nothing
  else in the stock rootfs references it — no other script, no other binary.
  Its document root and its supporting shared libraries are likewise named
  only by the web server itself.
- **How it manages to keep running after the script that launched it exits
  is not fully explained.** It does not daemonize (it stays in the
  foreground — see [the userland note](rtl9601-userland.md)), so on its own
  it should not outlive its parent; whether the boot script (or something
  else) respawns a killed instance is an open question.

## Evidence

Confirmed by tracing the boot script's control flow: the web server binary
is invoked from exactly one place, on the tail end of the failure path, with
no branch between "launch the web server" and "print a startup-failed
message." No other file in the stock image references the binary at all.

## See also

- [The stock userland's networking gaps](rtl9601-userland.md)
- [The free rc-script slot depends on the image](rtl9601-rc-script-slots-depend-on-base.md)
