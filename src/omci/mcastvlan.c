/* Class 309 attribute 16 as bridge rules. See mcastvlan.h. */
#include "mcastvlan.h"
#include "extvlan.h"

static void map_set(uint8_t *map, uint32_t vid)
{
	if (vid >= 1 && vid <= 4094)
		map[vid >> 3] |= (uint8_t)(1u << (vid & 7));
}

static int map_has(const uint8_t *map, unsigned vid)
{
	return map && (map[vid >> 3] >> (vid & 7) & 1u);
}

static void mark_act(uint8_t *map, const struct omci_vlan_act *a)
{
	if (a->tag_op == OMCI_TAGOP_PUSH || a->tag_op == OMCI_TAGOP_REWRITE)
		map_set(map, a->set_tag.vid);
}

void mcv_mark(uint8_t *map, const struct omci_vlan_oper *vr)
{
	const uint32_t named = OMCI_TAGF_VID | OMCI_TAGF_TCI;

	if (vr->filter.outer_mode & named)
		map_set(map, vr->filter.outer.vid);
	if (vr->filter.inner_mode & named)
		map_set(map, vr->filter.inner.vid);
	mark_act(map, &vr->outer_act);
	mark_act(map, &vr->inner_act);
	/* The tag a both-ways rule leaves on the PON side, which is what its
	 * downstream half matches (the manual add-tag rule, the VID filters). */
	map_set(map, vr->out.out_tag.vid);
}

static void skip(struct mcv_out *o, uint16_t vid, const char *why)
{
	for (int i = 0; i < o->nskip; i++)
		if (o->skip[i].vid == vid)
			return;
	if (o->nskip < MCV_SKIPS_MAX) {
		o->skip[o->nskip].vid = vid;
		o->skip[o->nskip].why = why;
		o->nskip++;
	}
}

/* The plan of one multicast VLAN: a frame with a C-tag of `vid` and no
 * S-tag, compared on that VID alone, and the treatment of `type`. */
static const char *plan(int type, unsigned vid, unsigned t_vid, unsigned t_pri,
			struct evtocd_plan *p)
{
	const char *kind = 0;

	for (unsigned i = 0; i < sizeof *p; i++)
		((uint8_t *)p)[i] = 0;
	p->cmp = EVTOCD_CMP_C;
	p->s.f_vid = -1;
	p->s.f_pri = -1;
	p->s.vid = -1;
	p->s.pri = -1;
	p->c.in = 1;
	p->c.out = 1;
	p->c.f_vid = (int16_t)vid;
	p->c.f_pri = -1;
	p->c.vid = -1;
	p->c.pri = -1;
	p->out_tags = 1;
	p->out_vid = (int16_t)vid;
	p->out_pri = -1;
	switch (type) {
	case MCV_STRIP:
		p->c.op = EVTOCD_OP_POP;
		p->c.out = 0;
		p->out_tags = 0;
		p->out_vid = -1;
		kind = "mcast-strip";
		break;
	case MCV_REPLACE:
		p->c.op = EVTOCD_OP_REWRITE;
		p->c.vid = (int16_t)t_vid;
		p->c.pri = (int8_t)t_pri;
		p->out_vid = (int16_t)t_vid;
		p->out_pri = (int8_t)t_pri;
		kind = "mcast-replace";
		break;
	case MCV_REPLACE_VID:
		/* Priority -1 on a C-tag REWRITE: the tag keeps its own
		 * (docs/SWITCH.md, "Extended VLAN rules"). */
		p->c.op = EVTOCD_OP_REWRITE;
		p->c.vid = (int16_t)t_vid;
		p->out_vid = (int16_t)t_vid;
		kind = "mcast-replace-vid";
		break;
	case MCV_ADD:
		/* The C-tag stays; the new tag goes in front of it, which the
		 * classifier can do only as an S-tag (TPID 0x88a8). */
		p->s.op = EVTOCD_OP_PUSH;
		p->s.out = 1;
		p->s.vid = (int16_t)t_vid;
		p->s.pri = (int8_t)t_pri;
		p->out_tags = 2;
		p->out_vid = (int16_t)t_vid;
		p->out_pri = (int8_t)t_pri;
		kind = "mcast-add";
		break;
	}
	return kind;
}

void mcv_plan(const struct mcv_in *in, struct mcv_out *o)
{
	int type = in->force_strip ? MCV_STRIP : in->tci[0];
	unsigned t_pri = in->tci[1] >> 5, t_dei = in->tci[1] >> 4 & 1u;
	unsigned t_vid = ((unsigned)in->tci[1] & 0x0fu) << 8 | in->tci[2];

	o->type = type;
	o->why = 0;
	o->nrule = 0;
	o->nskip = 0;
	switch (type) {
	case MCV_PASS:
		return;                  /* the VLAN rules of the line decide */
	case MCV_STRIP:
		break;
	case MCV_ADD:
	case MCV_REPLACE:
	case MCV_REPLACE_VID:
		if (t_vid > 4094) {
			o->why = "its TCI names VID 4095, which is not a VLAN";
			return;
		}
		if (t_dei && type != MCV_REPLACE_VID) {
			o->why = "its TCI sets DEI, which the classifier cannot write";
			return;
		}
		break;
	case MCV_ADD_PKG:
	case MCV_REPLACE_PKG:
	case MCV_REPLACE_VID_PKG:
		o->why = "types 5 to 7 take the VID of the service package table, "
			 "which does not select by VID here";
		return;
	default:
		o->why = "a reserved control type";
		return;
	}
	if (!in->nani) {
		o->why = "no access control list row names the VLAN its groups come in";
		return;
	}
	for (int i = 0; i < in->nani; i++) {
		unsigned a = in->ani[i];
		int seen = 0;

		for (int k = 0; k < i; k++)
			if (in->ani[k] == a)
				seen = 1;
		if (seen)
			continue;
		if (a == MCV_ANI_UNTAGGED) {
			skip(o, (uint16_t)a, "untagged groups: a rule on untagged frames "
			     "would take the unicast ones too");
			continue;
		}
		if (a == MCV_ANI_ANY_TAGGED || a == MCV_ANI_UNSPECIFIED || a > 4094) {
			skip(o, (uint16_t)a, "names no VLAN of its own");
			continue;
		}
		if (map_has(in->unicast, a)) {
			skip(o, (uint16_t)a, "a unicast rule of the line names this VLAN too");
			continue;
		}
		if (o->nrule >= MCV_RULES_MAX) {
			skip(o, (uint16_t)a, "more multicast VLANs than rules kept");
			continue;
		}
		{
			struct mcv_rule *r = &o->rule[o->nrule];
			struct evtocd_plan p;

			r->kind = plan(type, a, t_vid, t_pri, &p);
			r->vid = (uint16_t)a;
			evtocd_plan_oper(&p, &r->vr);
			r->vr.out.is_mcast = 1;
			o->nrule++;
		}
	}
}
