# Size a payload against jffs2's own behaviour, not against raw file size

The config partition compresses data on write and allocates whole 4 KB
erase blocks, so summing raw file sizes over-estimates space needed for
text by roughly 3x.

*Last verified: 2026-09-15*

---

## What

The config partition **compresses on write**, so deciding whether something
fits by summing raw file sizes over-estimates text payloads by about three
times. The underlying filesystem (jffs2) compresses each file, then
allocates whole **4 KB erase blocks** regardless of the compressed size. To
size a payload accurately: compress each file the same way the filesystem
does (a standard `gzip -6` is a close, slightly pessimistic estimate), round
each compressed size up to the nearest 4 KB block, and add them up.

## Why it matters

A sizing guard that summed raw file sizes once refused a payload that was
actually well within the free space available, because it estimated more
than three times the real requirement. Two consequences that bite in
opposite directions:

- **This filesystem is log-structured, so a re-deploy of the exact same
  payload can be refused even though it fit just fine the first time.**
  Overwriting a file does not free its old blocks until garbage collection
  runs. Deleting the old directory first frees the space immediately.
- Text compresses to roughly a third, so the partition holds far more than
  its raw size suggests — a payload of JavaScript, CSS and tab-separated
  data that looked much larger uncompressed occupied only a small fraction
  of that once written.

The free-space headroom on this partition is not just a nice-to-have: it
also holds device identity and any override files, so filling it puts
ordinary configuration writes at risk too.

## Evidence

The model is measured, not assumed: uploading a file of a known size moved
free space by exactly the size of its gzip-compressed form rounded up to
the next 4 KB block, matching the prediction exactly.

## See also

- [Config store](rtl9601-config-store.md)
- [NOR flash part](rtl9601-nor-flash-part.md) — 4 KB erase blocks are a
  property of this NOR part
