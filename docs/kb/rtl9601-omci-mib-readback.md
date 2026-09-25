# The CLI exposes the OLT's own provisioned view of the MIB, in a format that shifts between tables

The stock CLI can read back what the OLT actually provisioned, which is the ground truth over the device's raw config keys when the two disagree — but one particular subcommand deadlocks the daemon and must never be used.

*Last verified: 2026-09-12*

---

## What

The stock CLI on this firmware base exposes the provisioned MIB read-only, with subcommands to read a single managed-entity class or instance, list all registered tables, dump the active bridge connections / service flows / queue mappings / internal task list, and read individual attributes, current values, or alarms.

The firmware ships around 81 managed-entity plugin modules, which is the registered table list as far as the image is concerned. The device's own raw config keys are what the stick *asked for*; the MIB readback is what the line actually *built* — and when the two disagree, the MIB readback is the ground truth.

**The dump format is not consistent between tables, even within the same firmware.** Different tables use different capitalization for the same logical field, and different tables. Sub-table rows arrive as bare lines with no key at all — a heading, an index line, then several filter/treatment rows. A capture can end mid-entry, on a header with no row underneath it.

## Why it matters

Any parser for this output has to be written against real captures, not against an assumed shape. Assuming one capitalization, or that a header is always followed by a row, produces a parser that works on the table you tested and silently misreads the next one. The same caution applies to other diagnostic dump commands on this firmware, which can have several different header layouts depending on which variant is requested — match each row against the header immediately above it, rather than splitting by column position.

A "set" variant of the MIB-read command exists on the CLI and is worth leaving alone: the MIB is the OLT's own copy and is rebuilt on every re-registration, so a local write there survives one reboot at most.

**Never run the "list all registered tables" subcommand against the stock OMCI daemon.** (A replacement daemon is free to implement it correctly, and one has; the bug described here is specific to the stock binary, not to the command itself.) Against the stock daemon it returns **zero bytes** and leaves the MIB service unable to answer anything else — every later table read comes back empty — until the daemon is restarted. This was isolated cleanly: right after a fresh daemon restart, a normal table read works, the "list all tables" command is issued, and the same table read immediately after comes back empty. Other simple read commands issued in the same session (serial number, device mode, a config flag) were all unaffected, so the fault is specific to this one command, not to command volume or which process issued it.

**The cause is an internal double-lock in the stock daemon's MIB service.** The "list all tables" handler acquires an internal lock, prints each registered table name, and then acquires the *same* lock a second time — where the code path should instead release it. Because that lock is a plain, non-recursive mutex, the second acquisition deadlocks the daemon against itself, on the daemon's single message-processing thread. Every other similar command in the same service correctly pairs its lock and unlock; this one command does not.

Both parts of the observed symptom follow directly from that: the listing output is buffered and only flushed after the handler returns, which it never does — hence the zero bytes; and the deadlock hangs the daemon's only message-loop thread, so it never processes another message of any kind — hence every later read being empty. One consequence that has not been directly observed, but follows logically: OMCI frames from the OLT itself arrive on that same processing path, so after triggering this bug the ONU should also stop answering the line entirely. Forwarding continues regardless, since that lives in switch hardware, not in the daemon — which is presumably why the bug went unnoticed for a long time in normal use.

This is exactly the kind of command a management UI would call once on page load to populate a picker — which is how the bug tends to surface in practice: opening a MIB browser view can disable the very diagnostics that view exists to show. The safe approach is to capture the table names once (they only change with the firmware base) and treat that list as static data rather than querying it live.

Recovering costs nothing on the line: killing and restarting the OMCI daemon (using its normal startup script) rebuilds a byte-identical command line in about six seconds, and forwarding continues throughout the restart.

## Which UNI the OLT bridged, and how to read a termination-point type you cannot look up from a static table

**The MAC bridge port configuration table is the most informative table on the device for figuring out what the OLT actually did.** Each instance is one bridge port, with a termination-point type and pointer saying what the OLT attached to it. Two devices, same firmware, same stick model, provisioned by different OLTs:

| | ISP1 stick | ISP2 stick |
|---|---|---|
| bridge | one bridge, 8 ports | a different bridge, 3 ports |
| port 1 | physical Ethernet UNI | VEIP |
| port 2 | VEIP | a GEM interworking termination point |
| others | five GEM interworking termination points | one more |

So one OLT bridges the physical UNI *and* the VEIP; the other bridges the VEIP alone. That is the crux of most upstream discussion around VEIP-only provisioning, and it is a structural difference in what the OLT provisions, not a state flag to read off the ONU.

**Resolve the termination-point type by following its pointer to an actual managed entity on the device, not from a static table of type-number meanings.** Every termination-point pointer is the entity id of some other managed entity on the same stick — an Ethernet UNI entity, a VEIP entity, or a GEM interworking termination-point entity. Matching pointers to real entities checks the mapping against the device itself instead of against a specification from memory, and it tells you when you have encountered a type you have not yet pinned down, rather than silently guessing.

**Do not try to read forwarding status off the admin/oper-state fields on these bridge-port entities.** Two devices that both actively forward traffic have been observed holding *opposite* values in those fields on their own active bridge point. Whatever those fields mean in this context, it is not simply "working / not working," and any UI that renders one state as "enabled" is inventing an answer not supported by the data.

## See also

- [Forwarding visibility](rtl9601-forwarding-visibility.md) — the counters that say whether frames actually cross
- [Config store](rtl9601-config-store.md)
