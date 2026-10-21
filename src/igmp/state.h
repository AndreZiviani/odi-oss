/* What igmpd tells omcid about the subscriber's multicast, for ME 311: a
 * small text file, written whole and renamed over the old one when the group
 * table changes (the same arrangement as the policy file in the other
 * direction).
 *
 *   joins=N      join messages the subscriber sent (accepted or refused)
 *   denied=N     joins the profile refused: outside the lists, or past the
 *                group limit
 *   groups=N     groups the subscriber holds now
 *   group=A.B.C.D  one line a group, the static ones excluded: the table
 *                the OLT reads as the active group list
 *
 * Formatting is pure so the test can read it back without a filesystem.
 */
#ifndef ODI_IGMP_STATE_H
#define ODI_IGMP_STATE_H

#include <stdint.h>
#include "group.h"

#define IGMP_STATE_PATH       "/var/run/odi-mcast.state"
#define IGMP_STATE_TMP_PATH   "/var/run/odi-mcast.state.tmp"
/* Header lines and one line a group: 128 groups of up to 22 bytes
 * ("group=239.255.255.250\n"), with room to spare. */
#define IGMP_STATE_MAX        (3 * 32 + IGMP_MAX_GROUPS * 24)

/* Returns the length, or 0 when max is too small. */
uint32_t igmp_state_format(const struct igmp_db *db, char *buf, uint32_t max);

#endif
