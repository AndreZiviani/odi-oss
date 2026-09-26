/* `l2-table get all` and `l2-table get index <n>`, over /dev/odi_sw. */
#include "l2.h"
#include "hw.h"
#include "io.h"

static void print_row(const struct odi_sw_l2_row *r)
{
	char line[L2_LINE_MAX];

	l2_format_row(r, line);
	out(line);
	out_char('\n');
}

static void no_driver(int rc)
{
	out_fmt("%% the kernel has no L2 table readback on /dev/odi_sw (%d)\n", (long)rc);
}

int cmd_l2_all(void)
{
	struct odi_sw_l2_row r;
	uint32_t rows = 0, gip = 0, index = 0, n = 0;
	int rc = hw_l2_mode(&rows, &gip);

	if (rc != 0) {
		no_driver(rc);
		return 1;
	}
	out_fmt("L2 table: %u rows, IPv4 multicast looked up on %s\n",
		(unsigned long)rows, gip ? "the group address" : "MAC + VID/FID");
	out(l2_header);
	out_char('\n');
	while (index < rows) {
		rc = hw_l2_next(&index, &r);
		if (rc == 1)
			break;
		if (rc != 0) {
			out_fmt("%% L2 table read failed at row %u (%d)\n",
				(unsigned long)index, (long)rc);
			return 1;
		}
		print_row(&r);
		n++;
		index++;
	}
	out_fmt("%u %s\n", (unsigned long)n, n == 1 ? "entry" : "entries");
	return 0;
}

/* One row whatever its state, with the three raw words: the view for
 * checking the layout against the hardware, which is why an invalid row
 * is printed rather than skipped. */
int cmd_l2_index(uint32_t index)
{
	struct odi_sw_l2_row r;
	uint32_t rows = 0, gip = 0;
	int rc = hw_l2_get(index, &r);

	if (rc == -22) {
		if (hw_l2_mode(&rows, &gip) == 0)
			out_fmt("%% no such row: the table has %u (0 to %u)\n",
				(unsigned long)rows, (unsigned long)(rows - 1));
		else
			out("% no such row\n");
		return 1;
	}
	if (rc != 0) {
		no_driver(rc);
		return 1;
	}
	out(l2_header);
	out_char('\n');
	print_row(&r);
	out_fmt("valid %s  raw 0x", (r.flags & ODI_SW_L2_F_VALID) ? "yes" : "no");
	out_hex(r.raw[2], 8);
	out(" 0x");
	out_hex(r.raw[1], 8);
	out(" 0x");
	out_hex(r.raw[0], 8);
	out("  (bits 95..64 63..32 31..0)\n");
	return 0;
}
