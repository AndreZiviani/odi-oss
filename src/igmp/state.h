/* What igmpd tells omcid about the subscriber's multicast, for ME 311: a
 * small text file, written whole and renamed over the old one when the group
 * table changes (the same arrangement as the policy file in the other
 * direction).
 *
 *   joins=N      join messages the subscriber sent (accepted or refused)
 *   denied=N     joins the profile refused: outside the lists, or past the
 *                group limit
 *   groups=N     groups the subscriber holds now
 *   refused_acl=N, refused_limit=N, refused_full=N
 *                joins refused by the profile's lists, by its group limit,
 *                and for want of a table entry (igmpd's own limit); a line
 *                only once its count is above zero. Counted in denied: the
 *                first two. Nothing here is an OMCI figure; it is for
 *                `igmpd -s`.
 *   up_sent=N, up_withheld=N, up_limited=N, up_unsent=N
 *                while the reports are taken (uplink.h): messages sent up,
 *                withheld as unauthorized joins (or a query from the host
 *                side), dropped over the profile's rate, and not sent for want
 *                of a row, a flow or a transmit; a line only once its count is
 *                above zero. For `igmpd -s`, not OMCI.
 *   group=A.B.C.D  one line a group, the static ones excluded: the table
 *                the OLT reads as the active group list
 *   refused=REASON A.B.C.D vid=V at=T n=C
 *                the last few refusals, oldest first, after the groups so a
 *                reader that stops at the end of its buffer has everything
 *                it needs for ME 311 already. REASON is acl, limit or full;
 *                T is igmpd monotonic seconds; C how many times in a row.
 *
 * The file only ever grows by lines, and a reader skips what it does not
 * know and ignores a last line with no newline (the cut-off end of a file
 * longer than its buffer): omcid reads the first three keys and group lines.
 *
 * Formatting is pure so the test can read it back without a filesystem.
 */
#ifndef ODI_IGMP_STATE_H
#define ODI_IGMP_STATE_H

#include <stdint.h>
#include "group.h"

#define IGMP_STATE_PATH       "/var/run/odi-mcast.state"
#define IGMP_STATE_TMP_PATH   "/var/run/odi-mcast.state.tmp"
/* Header lines (up to twelve: the counts, the refusal counts and the way-up
 * counts), one line a group (128 of up to 22 bytes,
 * "group=239.255.255.250\n") and the refusal ring (up to 56 bytes a line,
 * "refused=limit 239.255.255.250 vid=4095 at=4294967295 n=4294967295\n"),
 * with room to spare. It has to stay under the 4096 bytes omcid reads. */
#define IGMP_STATE_MAX        (12 * 32 + IGMP_MAX_GROUPS * 24 + \
			       IGMP_REFUSAL_RING * 72)
typedef char igmp_state_fits_omcid_buffer[IGMP_STATE_MAX <= 4096 ? 1 : -1];

/* Returns the length, or 0 when max is too small. */
uint32_t igmp_state_format(const struct igmp_db *db, char *buf, uint32_t max);

/* A state file read back, for `igmpd -s`. */
struct igmp_state_view {
	uint32_t joins, denied, groups;
	uint32_t refused[IGMP_REFUSE_REASONS];
	uint32_t up_sent, up_withheld, up_limited, up_unsent;
	uint32_t above_version;
	uint32_t ngroup;
	uint32_t group[IGMP_MAX_GROUPS];
	uint32_t nrefusal;                      /* oldest first */
	struct igmp_refusal ref[IGMP_REFUSAL_RING];
};

/* Parse a state file's text. Unknown keys and malformed lines are skipped; a
 * last line without its newline is ignored (see above). Returns how many
 * lines it understood. */
int igmp_state_parse(const char *text, uint32_t len, struct igmp_state_view *v);

/* The name of a refusal reason ("acl", "limit", "full",
 * "preview"), "?" for others. */
const char *igmp_refusal_name(uint32_t reason);

#endif
