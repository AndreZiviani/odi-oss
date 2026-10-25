/* `acl-probe` over /dev/odi_sw. */
#include "acl_probe.h"
#include "table.h"
#include "hw.h"
#include "io.h"
#include "odi_sw_ioctl.h"

#define PROTO_IGMP 2u

static void clear(struct odi_sw_acl_probe *p)
{
	uint32_t *w = (uint32_t *)p;

	for (unsigned i = 0; i < sizeof *p / sizeof(uint32_t); i++)
		w[i] = 0;
}

static void out_ip(uint32_t a)
{
	out_udec((a >> 24) & 0xff);
	out_char('.');
	out_udec((a >> 16) & 0xff);
	out_char('.');
	out_udec((a >> 8) & 0xff);
	out_char('.');
	out_udec(a & 0xff);
}

static unsigned prefix_len(uint32_t mask)
{
	unsigned n = 0;

	while (n < 32 && (mask & (0x80000000u >> n)))
		n++;
	return n;
}

static void out_word(const char *key, uint32_t v)
{
	out(key);
	out("=0x");
	out_hex(v, 8);
}

static void print(const struct odi_sw_acl_probe *p)
{
	static const char *const action[] = { "?", "count", "mirror", "trap" };

	out("slot=");
	out_udec(p->slot);
	out(" entry=");
	out_udec(p->entry);
	out(" counter=");
	out_udec(p->counter);
	if (!p->in_use) {
		out(" empty\n");
		return;
	}
	out(" port=");
	out_udec(p->port);
	out(" action=");
	out(p->action <= ODI_SW_PROBE_TRAP ? action[p->action] : "?");
	out(p->bytes ? " unit=bytes" : " unit=frames");
	out(" proto=");
	if (p->proto == ODI_SW_PROBE_ANY)
		out("any");
	else
		out_udec(p->proto);
	out(" dst=");
	out_ip(p->dst);
	out_char('/');
	out_udec(prefix_len(p->dst_mask));
	out(" counted=");
	out_udec(p->now - p->base);
	out_char(' ');
	out_word("base", p->base);
	out_char(' ');
	out_word("now", p->now);
	out(" cpu=");
	out_udec(p->cpu_hits);
	out(" cpu_ports=0x");
	out_hex(p->cpu_ports, 1);
	out(" cpu_bytes=");
	out_udec(p->cpu_bytes);
	out(" uni_igmp_other=");
	out_udec(p->uni_igmp_other);
	out(" other_reason=");
	out_udec(p->other_reason);
	out_char('\n');
}

static int failed(int rc)
{
	switch (rc) {
	case -22:
		out("% refused: a slot is 0 to 7, a port 0 (UNI) or 2 (PON), a length 0 to 32;\n"
		    "% mirror and trap take IGMP from the UNI only\n");
		break;
	case -16:
		out("% busy: the slot is in use, another process holds the probes,\n"
		    "% or the IGMP snooping lease is held (igmpd)\n");
		break;
	case -25:
		out("% this kernel has no ACL probes on /dev/odi_sw\n");
		break;
	default:
		out_fmt("%% acl-probe failed (%d)\n", (long)rc);
		break;
	}
	return 1;
}

/* u: the slot, then for a count the port, then the protocol, or the group
 * and the prefix length. */
int cmd_probe_set(uint32_t sel, const uint32_t *u)
{
	struct odi_sw_acl_probe p;
	int rc;

	clear(&p);
	p.op = ODI_SW_PROBE_SET;
	p.slot = u[0];
	p.action = sel & PROBE_SEL_ACTION;
	p.bytes = (sel & PROBE_SEL_BYTES) ? 1u : 0u;
	if (p.action != ODI_SW_PROBE_COUNT) {
		p.port = 0;
		p.proto = PROTO_IGMP;
	} else if (sel & PROBE_SEL_GROUP) {
		p.port = u[1];
		p.proto = ODI_SW_PROBE_ANY;
		if (u[3] > 32)
			return failed(-22);
		p.dst_mask = u[3] ? 0xffffffffu << (32 - u[3]) : 0;
		p.dst = u[2] & p.dst_mask;
	} else {
		p.port = u[1];
		p.proto = u[2];
	}
	rc = hw_acl_probe(&p);
	if (rc)
		return failed(rc);
	p.in_use = 1;
	p.now = p.base;
	print(&p);
	return 0;
}

int cmd_probe_show(uint32_t slot)
{
	struct odi_sw_acl_probe p;
	int rc;

	clear(&p);
	p.op = ODI_SW_PROBE_GET;
	p.slot = slot;
	rc = hw_acl_probe(&p);
	if (rc)
		return failed(rc);
	print(&p);
	return 0;
}

int cmd_probe_delete(uint32_t slot)
{
	struct odi_sw_acl_probe p;
	int rc;

	clear(&p);
	p.op = ODI_SW_PROBE_DEL;
	p.slot = slot;
	rc = hw_acl_probe(&p);
	return rc ? failed(rc) : 0;
}
