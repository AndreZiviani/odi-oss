/* Buffered output and line input for the freestanding diag.
 *
 * Everything the CLI prints goes through one buffer, flushed on newline or when
 * full. Without it every keyword of a help listing would be its own write(2),
 * which on this device is the difference between a prompt that paints and one
 * that crawls. */
#ifndef ODI_IO_H
#define ODI_IO_H

#include <stdint.h>

void out_char(char c);
void out(const char *s);
void out_n(const char *s, int n);
void out_dec(long v);
void out_udec(unsigned long v);
void out_hex(unsigned long v, int width);
void out_flush(void);
/* Redirect flushed output; NULL restores stdout. The sink must buffer -- see
 * io.c. */
void out_set_sink(void (*fn)(const char *, int));

/* 64-bit decimal. Needed for the MIB counters, and worth its own routine
 * because dividing a uint64_t by ten would otherwise call into libgcc. */
void out_u64(uint64_t v);

/* Field-width helpers, matching the vendor's "%-35s" and "%25llu". */
void out_str_pad(const char *s, int width);
void out_u64_pad(uint64_t v, int width);

/* Supports %s %c %d %u %x %% only -- deliberately, since the vendor's format
 * strings are all this simple and a full printf is most of a libc. */
void out_fmt(const char *fmt, ...);

/* Read one line from fd into buf, stripping the trailing newline. Returns the
 * length, or -1 at end of input. */
int read_line(int fd, char *buf, int max);

int str_len(const char *s);
int str_eq(const char *a, const char *b);
void str_copy(char *dst, const char *src, int max);
/* Non-zero when `s` starts with `prefix`. For filename prefix tests, where
 * spelling the comparison out by hand has twice now produced a shorter prefix
 * than the comment beside it claimed. */
int str_has_prefix(const char *s, const char *prefix);

#endif
