/* The /tmp file the vendor's OMCI CLI answers through.
 *
 * `omci_open_cli_fd()` in libomci_mib.so removes /tmp/temp_omcicli* and then
 * dup2s a fresh one over stdout, so a dump arrives as a file rather than on
 * the socket. The stock client sleeps 12 ms, scans /tmp, and prints the first
 * match. Both halves of that contract are reproduced here -- omcid writes the
 * file, omcli reads one -- which is why the declarations are shared even
 * though the two scans do different jobs: omcid unlinks every match before
 * writing its own, omcli takes the first and stops.
 *
 * The prefix is the whole name, not `temp_`: the short one took anything else
 * in /tmp that happened to start temp_ with it.
 */
#ifndef OMCI_TMPFILE_H
#define OMCI_TMPFILE_H

#include <stdint.h>

#define OMCI_TMP_DIR     "/tmp"
#define OMCI_TMP_PREFIX  "temp_omcicli"
#define OMCI_TMP_OURS    "/tmp/temp_omcicli_omcid"

/* What getdents64 returns. Not in any header we have: the freestanding builds
 * link no libc, and this is the 64-bit variant the syscall actually fills in.
 * `d_name` is NUL-terminated within d_reclen. */
struct dirent64 {
	uint64_t d_ino;
	int64_t d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[];
};

/* "/tmp/" + name, truncated rather than overrun. Returns the length written. */
static inline int omci_tmp_path(char *dst, int max, const char *name)
{
	const char *pre = OMCI_TMP_DIR "/";
	int k = 0;

	while (*pre && k < max - 1)
		dst[k++] = *pre++;
	for (int i = 0; name[i] && k < max - 1; i++)
		dst[k++] = name[i];
	dst[k] = 0;
	return k;
}

#endif
