# The stock CLI can spin at 100% CPU if run non-interactively over SSH

Piping a single command into the stock `diag`-style CLI over SSH can leave it
spinning at 100% CPU indefinitely instead of exiting — avoid running it that
way against a stick carrying live service.

*Last verified: 2026-09-21*

---

## What

Piping a command into the stock firmware's switch/optics CLI over SSH (e.g.
`echo "gpon get alarm-status" | <cli>`) printed nothing, hung past a 60 second
timeout, and stayed runnable at high load until killed — it does not exit
cleanly when its stdin closes. Other stock CLI tools (the OMCI-focused ones)
returned normally over the same path.

Single observation, reproduced once.

## Why it matters

"Read-only" does not mean harmless on a production stick: a spinning CLI
process on this single-core device competes for CPU with the rest of the
firmware, including the OMCI stack that keeps the service up. Do not run the
stock switch/optics CLI on a live stick piped over SSH. If a value is needed,
prefer a tool that already knows how to call it safely (an OMCI-focused CLI,
a metrics exporter, or the web UI's own API), rather than invoking the raw
CLI non-interactively yourself.

## Evidence

Observed on one stick running the original (OEM) firmware: the process sat in
the running state at a load average of 5.9 for about two minutes before being
killed by hand; PPPoE and metrics collection were unaffected throughout, and
load returned to baseline within five minutes of the process being killed.
