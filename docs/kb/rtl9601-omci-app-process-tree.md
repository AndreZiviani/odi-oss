# The stock OMCI daemon is eight processes, not eight threads

Counting instances of the stock OMCI daemon by thread count or by pid clustering gives the wrong answer; count by process lineage instead.

*Last verified: 2026-09-16*

---

## What

The stock firmware's OMCI daemon process is **not** one process with eight threads. `/proc/<pid>/status` gives `Threads: 1` and a distinct `Tgid` for every one of them. It is a process tree — one parent at `PPid 1`, one child of it, and six grandchildren of that child.

And **`/proc/sys/kernel/pid_max` is 4096** on this platform, so pids wrap during an ordinary session.

## Why it matters

Two false alarms came from the same wrong model. **Counting "threads" fails, and so does judging whether the pids cluster.** Gate on lineage instead: collect the `PPid` of every matching pid; one instance is one process whose parent is 1, with everything else descending from it. Two parents at ppid 1 means two instances.

## Evidence

A healthy tree observed right after a restart:

    3888 (ppid 1) -> 4056 -> 4057, 4058, 362, 363, 364, 365

which looks like two instances and is one.

## See also

- [OMCI config changes apply without a reboot](rtl9601-omci-reapply-without-reboot.md)
