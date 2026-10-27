/* `igmpd -s`: what the daemon decided, readable by a person. The policy in
 * force, the groups held and the last refusals, from the two files the
 * daemon and omcid already keep (policy.h, state.h): nothing new is stored
 * for it and nothing an OLT sees depends on it.
 */
#ifndef ODI_IGMP_STATUS_H
#define ODI_IGMP_STATUS_H

#include <stdint.h>
#include "policy.h"
#include "mld.h"

/* The one-line summary the daemon logs whenever the policy changes. */
void igmp_policy_describe(const struct igmp_policy *p);

/* Print the report. `state` is the state file's text (len 0: there is none).
 * With `now` non-zero the refusals say how long ago they were; with 0 (a
 * file read offline, a transcript) they say the daemon second they carry. */
void igmp_status_print(const struct igmp_policy *p, int have_policy,
		       const char *state, uint32_t len, uint32_t now);

/* The IPv6 half (igmpd -6): its lists and the IPv6 state file's text. Says
 * nothing when there is neither, so a snooper without MLD reads as before. */
void mld_status_print(const struct mld_policy *p, const char *state, uint32_t len,
		      uint32_t now);

/* igmpd -s [STATE [POLICY [STATE6]]]: read the files (the daemon's own paths
 * when not named) and print. Returns the exit status. */
int igmp_status_run(const char *state_path, const char *policy_path,
		    const char *state6_path);

#endif
