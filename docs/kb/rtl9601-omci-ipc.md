# The CLI talks to the OMCI daemon over a System V message queue

The stock diagnostic CLI is a thin client over a System V message queue into the OMCI daemon; some commands answer over the queue directly, others through a temp file with no handshake, and one specific failure mode can reset the whole board.

*Last verified: 2026-09-15*

---

## What

The stock CLI holds essentially no logic of its own. Every command reduces to one of two calls that send a message to a **System V message queue**, created by the OMCI daemon at startup (the daemon owns the queue; the client only sends to it).

Each message is a fixed 20-byte header followed by a fixed 240-byte payload:

    header (20 bytes)
      +0  mtype     a constant identifying a CLI command    +12 replyKey   0 = fire and forget
      +4  reserved  0                                        +16 len       header + payload
      +8  msgType   one value = CLI command, another = a raw OMCI frame from the OLT

    payload: exactly 240 bytes, command id in word 0, bounds-checked against the command count

Sends are retried on a transient "queue full" condition only, a few times with a short backoff between attempts.

The daemon dispatches through a dense jump table indexed by command id, covering roughly 45 commands.

## How a command answers, and why it matters

- **A handful of commands answer on the queue itself**: the handler builds its own 240-byte reply and sends it back to a caller-specified reply queue.
- **About 19 commands answer through a temporary file**: the CLI process's output is redirected into a freshly created temp file (any stale file of the same naming pattern is cleared first), the handler's dump output goes into that file, and the file is flushed and the redirection restored. The dumping code streams output with no callback, no buffer, and no length prefix — the file is structural, not a stylistic choice.
- **The remaining commands (setters and debug triggers) answer nothing.**

**The CLI's only synchronization with the daemon is a fixed short sleep** (about 12 ms) before it scans for the temp file and prints it. There is no handshake, no length field, and no completion flag: a dump that takes longer than that sleep is printed truncated, which is what produces captures that end mid-entry with a header and no row under it. The reader never deletes the file itself; the *next* invocation's cleanup step does, so two overlapping CLI invocations can destroy or duplicate each other's output. The reply-queue key used for file-backed commands is also fixed rather than per-process, so two concurrent invocations can read each other's replies.

## What a more reliable client should do instead

The daemon's command handling is effectively single-threaded and every CLI message is dispatched in order, so CLI messages are strictly FIFO with respect to each other. **Sending a cheap queue-answering command immediately after a dump-producing one gives an exact completion signal**: that reply cannot arrive until the dump handler has returned and flushed its file, replacing the fixed sleep with a real synchronization point. A well-behaved client should also use a per-process reply-queue key, read the resulting file directly rather than shelling out to a generic file-dump command, and remove it afterwards. For attribute reads, one particular command returns the value as text directly on the queue with no file and no race, and is preferable when only a single attribute is needed.

## Two things the stock CLI does not expose

- **One valid command (a location-read command that returns raw managed-entity data on the queue) works but is not reachable through any keyword the stock CLI recognizes** — it exists in the command table but has no corresponding CLI verb.
- **A raw-frame-injection debug command exists** that accepts an arbitrary OMCI PDU and hands it to exactly the code path that handles frames arriving from the OLT, CRC and all — meaning anything able to write to this queue can present the ONU with any OMCI message the line could have sent: create or delete any managed entity, start a software download, or trigger a reboot. Because the queue's permissions restrict it to processes running as root, this is a root-only capability on stock firmware, and it is the cleanest way to exercise MIB behavior without an OLT present.

## A specific failure mode: dropping the OMCI redirect registration resets the board

Raw-OMCI-frame delivery (from the kernel/PON stack up to userland) is registered per redirect type and is **exclusive**: registering for a given redirect type replaces whoever previously held it rather than adding a second receiver. So any tool that registers for OMCI frame delivery takes that role away from the stock OMCI daemon until it registers again.

**This specific consequence — a full board reset — has been observed on the stock kernel and firmware only; it is not necessarily a property of a from-source replacement kernel.** A from-source replacement kernel has been observed, in the same scenario (a redirect registration dropped without being restored), to simply log an error and continue running, without resetting.

The proposed mechanism behind the stock kernel's reset is **inferred, not independently confirmed**: the stock kernel keeps attempting delivery to a registration that no longer exists, its failure path logs an unthrottled message on every dropped frame, and the OLT retries aggressively when a frame goes unanswered (one unacknowledged alarm walk was observed producing over 250 retried frames) — a plausible printk-storm-to-watchdog chain, given that a hardware watchdog is enabled early in boot and nothing in userland services it. What was directly **observed**, repeatedly: after a tool that had taken the OMCI redirect registration exited without restoring it, the stock stick went fully down (including ping) and came back roughly two minutes later.

So, on stock firmware, **any tool that takes this registration must give it back on the way out, including on a signal** — and note that `signal()` itself is not implemented in this kernel (it fails with "not implemented"); signal handlers must be installed through the lower-level `sigaction`-style call, whose parameter layout on this MIPS ABI puts the flags field before the handler pointer, and — because there is no C library signal-return trampoline available — a handler installed this way must not return; it should deregister and terminate the process directly.

Also worth noting operationally: run experiments of this kind **on the device itself**, not across a remote session — a cleanup command that is the last command of a remote session does not reliably run if the session itself is what drops.

## Evidence

The message-queue framing and temp-file behavior above were established by comparing what the CLI writes against what the daemon reads at every offset, both ends agreeing field for field. The 12 ms race itself has not been directly instrumented on hardware, but it is corroborated indirectly by real captures that end mid-entry exactly as that race predicts. The board-reset behavior, in contrast, was directly and repeatedly observed on live hardware.

## See also

- [OMCI MIB model](rtl9601-omci-mib-model.md)
