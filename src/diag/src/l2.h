/* The L2 table listing: `l2-table get all` and `l2-table get index <n>`.
 *
 * One line per valid row under one header, MAC first, and the header words
 * the stock listing uses for the same fields (MACAddress, Spa, Fid, Age, Vid,
 * State, Ext, Hash), so a reader that zips a row against its header -- the
 * odi-ui MAC table does -- reads either listing:
 *
 *   MACAddress        Spa Fid Age Vid  State  Ext Hash Type Ports Index
 *   78:54:2E:07:64:63 2   0   6   1    Auto   0   SVL  uc   -     0x008
 *   01:00:5E:01:02:03 -   -   -   10   Static -   IVL  mc   0x1   0x0a1
 *
 * A multicast row has no source port or age, and lists its member ports
 * instead; an IPv4 multicast route has no MAC and prints its group address
 * in the first column. The line builder is pure, so the host test drives it
 * without a device.
 */
#ifndef ODI_L2_H
#define ODI_L2_H

#include <stdint.h>
#include "odi_sw_ioctl.h"

#define L2_LINE_MAX 96

/* The header line, without a newline. */
extern const char l2_header[];

/* One row as a listing line, without a newline, into buf (at least
 * L2_LINE_MAX bytes). Returns the length. */
int l2_format_row(const struct odi_sw_l2_row *row, char *buf);

/* The commands. */
int cmd_l2_all(void);
int cmd_l2_index(uint32_t index);

#endif
