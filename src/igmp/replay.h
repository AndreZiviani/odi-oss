/* The seams between main.c (the daemon) and replay.c (a transcript driver):
 * what the daemon does once a second and once a frame, with the clock and
 * the policy file supplied by the caller. */
#ifndef ODI_IGMP_REPLAY_H
#define ODI_IGMP_REPLAY_H

#include <stdint.h>
#include "group.h"

/* main.c */
void igmpd_reset(struct igmp_db *db);
void igmpd_tick(struct igmp_db *db, uint32_t now, int first);
void igmpd_frame(struct igmp_db *db, const uint8_t *payload, uint32_t len,
		 uint32_t now);
/* Switch main.c to a transcript's clock (every later now_seconds() answers
 * `now`) and policy text (what the policy file would hold; the pointer must
 * stay valid). Nothing in a replay opens the device. */
void igmpd_replay_clock(uint32_t now);
void igmpd_replay_policy(const char *text, uint32_t len);
void igmpd_state_dump(const struct igmp_db *db);
/* What `igmpd -s` prints for this table and the policy in force. */
void igmpd_status_dump(const struct igmp_db *db);

/* replay.c: run FILE ("-" for stdin). Returns the exit status. */
int igmp_replay(const char *path);

#endif
