/* Do not take a redirect type somebody else is using.
 *
 * Registering for a packet-redirect type overwrites whoever held it
 * (odi_omci.c odi_omci_reg_set keeps one pid per type), and deregistering
 * clears the type outright, not just our own claim. So a second OMCI
 * receiver started by accident -- `omcicap --help` was one -- silently
 * steals the channel from omcid, and when it exits nobody holds it at all:
 * the ONU stops answering the OLT until omcid is restarted.
 *
 * redirect_guard() reads /proc/odi_omci before a tool registers. When a
 * live process other than the caller holds the type, it waits a few
 * seconds for that process to let go (`killall omcid; omcid ...` races the
 * old daemon deregistering on SIGTERM), then refuses unless the caller
 * passed force. A holder pid with no /proc entry is a stale registration
 * left by a process that died without deregistering, and is not in the
 * way. An unreadable /proc/odi_omci (no odi_omci driver, or qemu-user) is
 * reported and not treated as a refusal: there is nothing to protect there.
 */
#ifndef ODI_OMCI_REDIRECT_GUARD_H
#define ODI_OMCI_REDIRECT_GUARD_H

#include "sys.h"
#include "io.h"
#include "procparse.h"

#define RG_WAIT_TICKS 30              /* 100 ms each */

/* 1 held (pid set), 0 free, -1 cannot tell. */
static inline int rg_holder(unsigned type, uint32_t *pid)
{
	static char buf[512];
	long fd = sys_open("/proc/odi_omci", O_RDONLY), n;

	if (fd < 0)
		return -1;
	n = sys_read((int)fd, buf, sizeof buf);
	sys_close((int)fd);
	if (n <= 0)
		return -1;
	return pp_redirect_holder(buf, (unsigned)n, type, pid);
}

/* "/proc/<pid>/comm" into path. */
static inline void rg_comm_path(char *path, uint32_t pid)
{
	char digits[12];
	int n = 0, k = 0;
	const char *pre = "/proc/", *suf = "/comm";

	do {
		digits[n++] = (char)('0' + pid % 10);
		pid /= 10;
	} while (pid && n < (int)sizeof digits);
	while (*pre)
		path[k++] = *pre++;
	while (n)
		path[k++] = digits[--n];
	while (*suf)
		path[k++] = *suf++;
	path[k] = 0;
}

/* The holder's command name into name (NUL-terminated); 0 if it is alive,
 * -1 if there is no such process. */
static inline int rg_comm(uint32_t pid, char *name, int max)
{
	char path[32];
	long fd, n;

	rg_comm_path(path, pid);
	fd = sys_open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = sys_read((int)fd, name, (unsigned long)(max - 1));
	sys_close((int)fd);
	if (n < 0)
		n = 0;
	while (n > 0 && (name[n - 1] == '\n' || name[n - 1] == 0))
		n--;
	name[n] = 0;
	return 0;
}

/* Returns 0 when the caller may register for `type`, 1 when it must not
 * (the reason is already printed). `self` is the pid the caller registers
 * with; a registration that is already ours is no conflict. */
static inline int redirect_guard(const char *tool, unsigned type, int force,
				 uint32_t self)
{
	uint32_t pid = 0;
	char name[20];
	int held = 0;

	for (int tick = 0; tick <= RG_WAIT_TICKS; tick++) {
		int rc = rg_holder(type, &pid);

		if (rc < 0) {
			out_fmt("%s: cannot read /proc/odi_omci; not checking "
				"who holds redirect type %d\n", tool, (long)type);
			return 0;
		}
		if (rc == 0 || pid == self)
			return 0;
		if (rg_comm(pid, name, sizeof name) != 0)
			return 0;       /* stale: the holder is gone */
		held = 1;
		if (force)
			break;
		sys_nanosleep(0, 100000000);
	}
	if (!held)
		return 0;
	if (force) {
		out_fmt("%s: -f: taking redirect type %d from pid %d (%s); "
			"restart it afterwards\n", tool, (long)type, (long)pid, name);
		return 0;
	}
	out_fmt("%s: redirect type %d is held by pid %d (%s).\n"
		"Registering would take the channel from it, and exiting would "
		"leave the type with no receiver at all.\n"
		"Stop it first, or pass -f to take the channel anyway.\n",
		tool, (long)type, (long)pid, name);
	return 1;
}

#endif
