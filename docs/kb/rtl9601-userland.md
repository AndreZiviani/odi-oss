# The stock web server can't run CGI, and inetd is unusable — serve your own port instead

Running your own network service on the stock firmware means bringing your
own listener: the stock web server has no working external-CGI path, and
`inetd` can't be reconfigured because its config lives on the read-only
partition.

*Last verified: 2026-09-16*

---

## What

The stock userland gives you less than it appears to:

- **The stock web server (boa 0.93.15) has no working external-CGI path.**
  A request for anything registered as `application/x-httpd-cgi` returns 404,
  even though the binary contains the full CGI/1.1 environment (`GATEWAY_INTERFACE`,
  `REQUEST_METHOD`, `SCRIPT_NAME`, `QUERY_STRING`, `PATH_INFO`) and an `execve`
  call. Those strings serve the web server's own built-in form handlers, not
  external scripts — presence of a symbol is not presence of a working code
  path. The stock web UI's own dynamic pages are HTML templates processed
  in-process, not executed as separate CGI programs, which is consistent with
  external CGI simply not working: if it did, the discriminator is that
  removing the CGI MIME-type registration serves the same file as a plain
  static download (200) instead of 404.
- **The stock web server does not daemonize.** It stays in the foreground of
  whatever launched it.
- **`inetd` is unusable for adding services.** Its configuration path is
  fixed to a file on the read-only root filesystem, so there is no writable
  place to add an entry.

A small freestanding HTTP listener (on the order of 40 lines against the raw
syscall layer) avoids all three problems. A working metrics exporter built
this way came out under 3 KB.

The stock web server is started from a boot script by binary, not a shell
script — so its port cannot be changed by editing an init script; see
[boa is only launched from the failure path](rtl9601-boa-launched-from-startup-failure-path.md).
Running a second instance of the stock web server against a different config
root and port does work, which is a useful way to isolate behaviour without
touching the stick's management interface.

## Why it matters

If you plan to run your own listener, two device-specific pitfalls matter:

- **MIPS socket constants differ from x86/ARM and are silent when wrong** —
  `SOCK_STREAM` and `SOCK_DGRAM` are swapped, and `SOL_SOCKET` has a different
  value. See
  [MIPS o32 ABI traps](mips-o32-syscall-abi-traps.md) for this and other traps
  that are not specific to this device but bite here just the same.
- **The CPU is big-endian**, so host byte order already matches network byte
  order and a from-scratch listener needs no `htons` at all. This does not
  hold if you are targeting a little-endian MIPS variant.

## See also

- [boa is only launched from the failure path](rtl9601-boa-launched-from-startup-failure-path.md)
- [Stock busybox is missing several applets](rtl9601-busybox-missing-applets.md)
- [The rootfs is read-only; /etc, /tmp are symlinks into RAM](rtl9601-rootfs-symlinks-into-var.md)
- [Firmware is reversible across two partitions](rtl9601-dual-firmware-partitions.md)
- [The free rc-script slot depends on the image](rtl9601-rc-script-slots-depend-on-base.md)
- [The vendor kernel has no devpts, only legacy BSD ptys](rtl9601-legacy-bsd-ptys-only.md)
- [omci_app is one process tree, not eight threads](rtl9601-omci-app-process-tree.md)
- [No clock, no syslog on the stock image](rtl9601-no-clock-no-syslog.md)
- [MIPS o32 ABI traps](mips-o32-syscall-abi-traps.md)
- [The RLX5281 ISA, measured by execution](rtl9601-isa-map.md)
- [A current toolchain builds for this core](rtl9601-stock-gcc-cross-compile.md)
- [Moving files on and off the stick](rtl9601-netcat-file-transfer.md)
- [Config is jffs2 files, not what `flash` implies](rtl9601-config-store.md)
