/* `voip`: the voice lines the OLT provisioned, resolved.
 *
 * What an OLT provisions for a phone line is spread over a dozen entities
 * that point at each other: a voice CTP names the SIP user and the POTS UNI,
 * the SIP user names the agent, the number and the login, the agent names
 * its servers through network addresses, and so on. This follows the
 * pointers and prints one block per line, so nobody has to chase them
 * through `mib get`.
 *
 *   voip                 the view, with the number, the display name and the
 *                        login name masked and no password
 *   voip --secrets       the same with all of them in clear: the one place a
 *                        SIP password is printed
 *   voip --secrets-list  only the secret strings, one per line, for a scrub
 *                        list (the diag bundle)
 *
 * A 148 is a SIP credential only when something SIP points at it (the SIP
 * user, or the address of one of the agent's servers): the same class also
 * holds the login of the TR-069 server, which is not ours to print here, so
 * it is reached by pointer and never by class.
 *
 * Any user:password@ in a server URL is stripped before the URL is printed,
 * in every mode; the parts go to the secret list instead.
 */
#include "omcid.h"
#include "../extvlan.h"

#define CLS_TCP_UDP_CFG     136
#define CLS_NETWORK_ADDRESS 137

#define NIL        0xffffu
#define STR_MAX    384          /* a large string is up to 15 parts of 25 */
#define LINES_MAX  8

enum { V_MASKED, V_SECRETS, V_LIST };
static int vmode;

/* Nothing but the secrets in list mode. */
#define SAY(...) do { if (vmode != V_LIST) out_fmt(__VA_ARGS__); } while (0)

static int exists(uint16_t cls, uint32_t inst)
{
	return inst != NIL && (mib_find(cls, (uint16_t)inst) ||
			       instance_is_autonomous(cls, (uint16_t)inst));
}

/* A numeric attribute as a Get returns it. */
static uint32_t num(uint16_t cls, uint32_t inst, unsigned k)
{
	const struct omci_class *c = find_class(cls);
	uint8_t b[32];
	uint16_t w;
	uint32_t v = 0;

	if (!c || !(w = attr_width(c, k)) || w > sizeof b)
		return 0;
	attr_value(c, (uint16_t)inst, k, b);
	for (uint16_t i = 0; i < w && i < 4; i++)
		v = (v << 8) | b[i];
	return v;
}

/* Text kept to what a terminal shows; the OLT chooses these bytes. */
static void clean(char *s)
{
	for (; *s; s++)
		if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7e)
			*s = '?';
}

/* A string attribute, NUL-terminated, appended at dst + at. */
static unsigned text(uint16_t cls, uint32_t inst, unsigned k, char *dst,
		     unsigned at, unsigned max)
{
	const struct omci_class *c = find_class(cls);
	uint8_t b[32];
	uint16_t w;

	if (!c || !(w = attr_width(c, k)) || w > sizeof b) {
		dst[at] = 0;
		return at;
	}
	attr_value(c, (uint16_t)inst, k, b);
	for (uint16_t i = 0; i < w && b[i] && at + 1 < max; i++)
		dst[at++] = (char)b[i];
	dst[at] = 0;
	clean(dst);
	return at;
}

/* A large string (157), whole: up to 15 parts, a NUL ends it. 0 when there
 * is no such entity. */
static int large(uint32_t inst, char *dst, unsigned max)
{
	const struct omci_class *c = find_class(OMCI_ME_LARGE_STRING);
	unsigned n = 0, parts;
	uint8_t b[32];

	dst[0] = 0;
	if (!c || !exists(OMCI_ME_LARGE_STRING, inst))
		return 0;
	parts = num(OMCI_ME_LARGE_STRING, inst, 1);
	if (parts > 15)
		parts = 15;
	for (unsigned p = 1; p <= parts; p++) {
		uint16_t w = attr_width(c, 1 + p);
		uint16_t i;

		if (!w || w > sizeof b)
			break;
		attr_value(c, (uint16_t)inst, 1 + p, b);
		for (i = 0; i < w && b[i] && n + 1 < max; i++)
			dst[n++] = (char)b[i];
		if (i < w)
			break;
	}
	dst[n] = 0;
	clean(dst);
	return 1;
}

/* A secret string: on the list in list mode, nowhere else. */
static void secret(const char *s)
{
	if (vmode == V_LIST && *s) {
		out(s);
		out_char('\n');
	}
}

/* The same masking the diag bundle gives the serial: MASKED and the last
 * four characters, which still tell two lines apart, or MASKED alone for a
 * value too short to give four away. */
static void shown(const char *s)
{
	int n = str_len(s);

	secret(s);
	if (vmode == V_LIST)
		return;
	if (!n)
		out("-");
	else if (vmode == V_SECRETS)
		out(s);
	else if (n >= 10)
		out_fmt("MASKED-%s", s + n - 4);
	else
		out("MASKED");
}

/* ------------------------------------------------------------ server URLs */

static int scheme_known(const char *s, int n)
{
	static const char *const known[] = { "sip", "sips", "tel", "udp", "tcp",
					     "http", "https", 0 };

	for (int k = 0; known[k]; k++) {
		int i = 0;

		for (; i < n && known[k][i]; i++)
			if ((s[i] | 0x20) != known[k][i])
				break;
		if (i == n && !known[k][i])
			return 1;
	}
	return 0;
}

/* The URL without its user:password@ (nothing else is touched); the two
 * halves of what was cut go to the secret list. */
static void strip_userinfo(const char *s, char *dst, unsigned max)
{
	int start = 0, at = -1, i = 0;
	unsigned n = 0;

	while (s[i] && s[i] != ':' && s[i] != '@' && s[i] != '/')
		i++;
	if (s[i] == ':') {
		if (s[i + 1] == '/' && s[i + 2] == '/')
			start = i + 3;
		else if (scheme_known(s, i))
			start = i + 1;
	}
	for (i = start; s[i] && s[i] != '/' && s[i] != '?'; i++)
		if (s[i] == '@') {
			at = i;
			break;
		}
	if (at < 0) {
		for (; s[n] && n + 1 < max; n++)
			dst[n] = s[n];
		dst[n] = 0;
		return;
	}
	for (i = 0; i < start && n + 1 < max; i++)
		dst[n++] = s[i];
	for (i = at + 1; s[i] && n + 1 < max; i++)
		dst[n++] = s[i];
	dst[n] = 0;
	if (vmode == V_LIST) {
		/* user, then password, if there is one */
		for (i = start; i < at && s[i] != ':'; i++)
			out_char(s[i]);
		out_char('\n');
		if (i < at) {
			for (i++; i < at; i++)
				out_char(s[i]);
			out_char('\n');
		}
	}
}

/* ------------------------------------------------------------- credentials */

/* An authentication security method (148) that something SIP points at. */
static void auth(const char *what, uint32_t id)
{
	static char user[64], realm[32], pw[32];
	unsigned n;

	if (!exists(OMCI_ME_AUTH_SECURITY_METHOD, id))
		return;
	n = text(OMCI_ME_AUTH_SECURITY_METHOD, id, 2, user, 0, sizeof user);
	/* Username 2 carries the rest of a name longer than 25 bytes. */
	if (n >= 25)
		text(OMCI_ME_AUTH_SECURITY_METHOD, id, 5, user, n, sizeof user);
	text(OMCI_ME_AUTH_SECURITY_METHOD, id, 3, pw, 0, sizeof pw);
	text(OMCI_ME_AUTH_SECURITY_METHOD, id, 4, realm, 0, sizeof realm);
	secret(pw);
	if (vmode == V_LIST) {
		secret(user);
		return;
	}
	SAY("  %s login 0x%x: username ", what, (unsigned long)id);
	shown(user);
	if (vmode == V_SECRETS)
		SAY(", password %s", *pw ? pw : "-");
	else if (*pw)
		SAY(", password set (%d bytes)", (long)str_len(pw));
	else
		SAY(", password not set");
	SAY(", realm %s\n", *realm ? realm : "-");
}

/* A server: a network address (137) whose address is a large string, or a
 * large string named directly. */
static void server(const char *label, uint32_t ptr)
{
	static char url[STR_MAX], bare[STR_MAX];

	if (exists(CLS_NETWORK_ADDRESS, ptr)) {
		large(num(CLS_NETWORK_ADDRESS, ptr, 2), url, sizeof url);
		auth(label, num(CLS_NETWORK_ADDRESS, ptr, 1));
	} else if (!large(ptr, url, sizeof url)) {
		SAY("    %s -\n", label);
		return;
	}
	strip_userinfo(url, bare, sizeof bare);
	SAY("    %s %s\n", label, *bare ? bare : "-");
}

/* ------------------------------------------------------------- the VLAN */

/* The tag the OLT puts on what the IP host sends: the first extended VLAN
 * rule that adds one, for the host or for the bridge port it hangs on; failing
 * that the first VLAN a filter of that bridge port lets through. */
static int vlan_of(uint32_t host, unsigned *vid, unsigned *pri)
{
	const struct omci_class *c47 = find_class(OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA);
	const struct omci_class *c171 = find_class(OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA);
	const struct omci_class *c84 = find_class(OMCI_ME_VLAN_TAGGING_FILTER_DATA);
	uint32_t bp = NIL;

	if (!c47 || !c171 || !c84)
		return 0;
	for (int i = 0; i < MIB_ROWS; i++) {
		const struct mib_row *r = &mib[i];

		if (r->used && r->classId == OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA &&
		    row_u32(r, c47, 3) == MBPCD_TP_IP_HOST &&
		    row_u32(r, c47, 4) == host) {
			bp = r->inst;
			break;
		}
	}
	for (int i = 0; i < MIB_ROWS; i++) {
		const struct mib_row *r = &mib[i];
		uint32_t type, ptr;

		if (!r->used || r->classId != OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA)
			continue;
		type = row_u32(r, c171, 1);
		ptr = row_u32(r, c171, 7);
		/* association type 3 is an IP host, 0 a bridge port */
		if (!((type == 3 && ptr == host) ||
		      (type == 0 && bp != NIL && ptr == bp)))
			continue;
		for (uint8_t k = r->tbl_head; k != MIB_TBL_NONE; k = tblpool[k].next) {
			struct evtocd_entry e;

			if (tblpool[k].attr != 6)
				continue;
			evtocd_decode(tblpool[k].data, &e);
			if (e.remove_tags == EVTOCD_T_DISCARD_FRAME)
				continue;
			if (e.t_out_pri != EVTOCD_T_PRI_DO_NOT_ADD &&
			    e.t_out_vid <= EVTOCD_VID_MAX) {
				*vid = e.t_out_vid;
				*pri = e.t_out_pri;
				return 1;
			}
			if (e.t_in_pri != EVTOCD_T_PRI_DO_NOT_ADD &&
			    e.t_in_vid <= EVTOCD_VID_MAX) {
				*vid = e.t_in_vid;
				*pri = e.t_in_pri;
				return 1;
			}
		}
	}
	if (bp != NIL && exists(OMCI_ME_VLAN_TAGGING_FILTER_DATA, bp)) {
		uint8_t b[32];

		attr_value(c84, (uint16_t)bp, 1, b);
		for (int i = 0; i + 1 < 24; i += 2) {
			unsigned tci = (unsigned)(b[i] << 8 | b[i + 1]);

			if (tci & 0xfff) {
				*vid = tci & 0xfff;
				*pri = tci >> 13;
				return 1;
			}
		}
	}
	return 0;
}

/* ------------------------------------------------------------ one line */

static const char *codec_name(unsigned pt)
{
	switch (pt) {
	case 0:  return "PCMU";
	case 3:  return "GSM";
	case 4:  return "G723";
	case 8:  return "PCMA";
	case 9:  return "G722";
	case 18: return "G729";
	default: return 0;
	}
}

static const char *status_name(unsigned s)
{
	switch (s) {
	case 0:  return "none (initial)";
	case 1:  return "registered";
	case 2:  return "in session";
	case 3:  return "registration failed (icmp error)";
	case 4:  return "registration failed (tcp error)";
	case 5:  return "registration failed (authentication)";
	case 6:  return "registration failed (timeout)";
	case 7:  return "registration failed (server fail code)";
	case 8:  return "invite failed (icmp error)";
	case 9:  return "invite failed (tcp error)";
	case 10: return "invite failed (authentication)";
	case 11: return "invite failed (timeout)";
	case 12: return "invite failed (server fail code)";
	case 13: return "port not configured";
	case 14: return "config done";
	default: return 0;
	}
}

/* The SIP agent; returns the IP host its TCP/UDP data names, or NIL. */
static uint32_t agent(uint32_t a)
{
	static char host[STR_MAX], bare[STR_MAX];
	uint32_t t = num(OMCI_ME_SIP_AGENT_CFG_DATA, a, 5), ip_host = NIL;

	SAY("  sip agent 0x%x\n", (unsigned long)a);
	server("proxy", num(OMCI_ME_SIP_AGENT_CFG_DATA, a, 1));
	server("outbound proxy", num(OMCI_ME_SIP_AGENT_CFG_DATA, a, 2));
	server("registrar", num(OMCI_ME_SIP_AGENT_CFG_DATA, a, 10));
	large(num(OMCI_ME_SIP_AGENT_CFG_DATA, a, 8), host, sizeof host);
	strip_userinfo(host, bare, sizeof bare);
	SAY("    host part %s\n", *bare ? bare : "-");
	if (exists(CLS_TCP_UDP_CFG, t)) {
		unsigned proto = num(CLS_TCP_UDP_CFG, t, 2);

		SAY("    port %u/%s\n", (unsigned long)num(CLS_TCP_UDP_CFG, t, 1),
		    proto == 6 ? "tcp" : proto == 17 ? "udp" : "?");
		ip_host = num(CLS_TCP_UDP_CFG, t, 4);
	}
	for (unsigned k = 3; k <= 4; k++) {
		uint32_t d = num(OMCI_ME_SIP_AGENT_CFG_DATA, a, k);

		if (d)
			SAY("    dns %u.%u.%u.%u\n", (unsigned long)(d >> 24),
			    (unsigned long)((d >> 16) & 0xff),
			    (unsigned long)((d >> 8) & 0xff), (unsigned long)(d & 0xff));
	}
	SAY("    expiry %u s\n",
	    (unsigned long)num(OMCI_ME_SIP_AGENT_CFG_DATA, a, 6));
	return ip_host;
}

static void media(uint32_t m)
{
	static const unsigned codec_attr[4] = { 3, 6, 9, 12 };
	uint32_t vsp = num(OMCI_ME_VOIP_MEDIA_PROFILE, m, 2);
	uint32_t rtp = num(OMCI_ME_VOIP_MEDIA_PROFILE, m, 16);

	SAY("  media profile 0x%x: codecs", (unsigned long)m);
	for (int i = 0; i < 4; i++) {
		unsigned pt = num(OMCI_ME_VOIP_MEDIA_PROFILE, m, codec_attr[i]);
		unsigned ms = num(OMCI_ME_VOIP_MEDIA_PROFILE, m, codec_attr[i] + 1);
		const char *nm = codec_name(pt);

		/* a slot nobody filled reads as codec 0, period 0 */
		if (i && !ms && !pt)
			continue;
		if (nm)
			SAY(" %s/%ums", nm, (unsigned long)ms);
		else
			SAY(" pt%u/%ums", (unsigned long)pt, (unsigned long)ms);
	}
	SAY(", dtmf %s\n", num(OMCI_ME_VOIP_MEDIA_PROFILE, m, 15) ?
	    "out of band" : "in band");
	if (exists(OMCI_ME_RTP_PROFILE_DATA, rtp))
		SAY("    rtp ports %u-%u dscp %u\n",
		    (unsigned long)num(OMCI_ME_RTP_PROFILE_DATA, rtp, 1),
		    (unsigned long)num(OMCI_ME_RTP_PROFILE_DATA, rtp, 2),
		    (unsigned long)num(OMCI_ME_RTP_PROFILE_DATA, rtp, 3));
	if (exists(OMCI_ME_VOICE_SERVICE_PROFILE, vsp))
		SAY("    voice service profile 0x%x: jitter target %u max %u ms, "
		    "echo cancel %s\n", (unsigned long)vsp,
		    (unsigned long)num(OMCI_ME_VOICE_SERVICE_PROFILE, vsp, 2),
		    (unsigned long)num(OMCI_ME_VOICE_SERVICE_PROFILE, vsp, 3),
		    num(OMCI_ME_VOICE_SERVICE_PROFILE, vsp, 4) ? "on" : "off");
}

/* One SIP user and everything it reaches. */
static void user(uint32_t u, uint32_t media_ptr)
{
	static char aor[STR_MAX], name[32];
	uint32_t a = num(OMCI_ME_SIP_USER_DATA, u, 1), ip_host;
	uint32_t app = num(OMCI_ME_SIP_USER_DATA, u, 8);
	unsigned vid, pri;

	large(num(OMCI_ME_SIP_USER_DATA, u, 2), aor, sizeof aor);
	text(OMCI_ME_SIP_USER_DATA, u, 3, name, 0, sizeof name);
	SAY("  sip user 0x%x: aor ", (unsigned long)u);
	shown(aor);
	SAY(", display name ");
	shown(name);
	SAY("\n");
	auth("sip user", num(OMCI_ME_SIP_USER_DATA, u, 4));
	if (exists(OMCI_ME_SIP_AGENT_CFG_DATA, a))
		ip_host = agent(a);
	else {
		SAY("  sip agent -\n");
		ip_host = NIL;
	}
	if (media_ptr != NIL && exists(OMCI_ME_VOIP_MEDIA_PROFILE, media_ptr)) {
		media(media_ptr);
		if (ip_host == NIL)
			ip_host = num(OMCI_ME_RTP_PROFILE_DATA,
				      num(OMCI_ME_VOIP_MEDIA_PROFILE, media_ptr, 16), 8);
	}
	if (ip_host == NIL && exists(OMCI_ME_VOIP_APP_SERVICE_PROFILE, app))
		ip_host = num(OMCI_ME_VOIP_APP_SERVICE_PROFILE, app, 10);
	if (!exists(OMCI_ME_IP_HOST_CONFIG_DATA, ip_host))
		SAY("  voice vlan: no IP host named\n");
	else if (vlan_of(ip_host, &vid, &pri) && pri <= EVTOCD_PRI_MAX)
		SAY("  voice vlan %u priority %u (IP host 0x%x)\n",
		    (unsigned long)vid, (unsigned long)pri, (unsigned long)ip_host);
	else if (vlan_of(ip_host, &vid, &pri))
		SAY("  voice vlan %u, priority taken from the frame (IP host 0x%x)\n",
		    (unsigned long)vid, (unsigned long)ip_host);
	else
		SAY("  voice vlan: untagged (IP host 0x%x)\n", (unsigned long)ip_host);
}

static void status(uint32_t port)
{
	unsigned s;
	const char *nm;

	if (!exists(OMCI_ME_VOIP_LINE_STATUS, port)) {
		SAY("  line status -\n");
		return;
	}
	s = num(OMCI_ME_VOIP_LINE_STATUS, port, 2);
	nm = status_name(s);
	if (nm)
		SAY("  line status %s (%u)\n", nm, (unsigned long)s);
	else
		SAY("  line status code %u\n", (unsigned long)s);
}

static uint16_t ctps[LINES_MAX], users[LINES_MAX];

static unsigned collect(uint16_t cls, uint16_t *dst)
{
	const struct mib_ent *e;
	int n = mib_entities(cls, 0, 0, &e), k = 0;

	for (int i = 0; i < n && k < LINES_MAX; i++)
		dst[k++] = e[i].inst;
	return (unsigned)k;
}

uint32_t cli_voip(void)
{
	unsigned nctp, nuser;

	vmode = V_MASKED;
	for (unsigned i = 1; cli_arg(i); i++) {
		if (str_eq(cli_arg(i), "--secrets")) {
			if (vmode == V_MASKED)
				vmode = V_SECRETS;
		} else if (str_eq(cli_arg(i), "--secrets-list")) {
			vmode = V_LIST;
		} else {
			out_fmt("voip: unknown argument %s\n", cli_arg(i));
			return OMCLI_EARGS;
		}
	}
	nctp = collect(OMCI_ME_VOIP_VOICE_CTP, ctps);
	nuser = collect(OMCI_ME_SIP_USER_DATA, users);
	if (!nctp && !nuser) {
		SAY("no voice provisioned\n");
		return OMCLI_OK;
	}
	for (unsigned i = 0; i < nctp; i++) {
		uint32_t port = num(OMCI_ME_VOIP_VOICE_CTP, ctps[i], 2);
		uint32_t u = num(OMCI_ME_VOIP_VOICE_CTP, ctps[i], 1);

		SAY("port 0x%x (voice CTP 0x%x)\n", (unsigned long)port,
		    (unsigned long)ctps[i]);
		if (exists(OMCI_ME_SIP_USER_DATA, u))
			user(u, num(OMCI_ME_VOIP_VOICE_CTP, ctps[i], 3));
		else
			SAY("  sip user -\n");
		status(port);
	}
	/* A SIP user no voice CTP names yet: provisioned, not on a port. */
	for (unsigned i = 0; i < nuser; i++) {
		unsigned used = 0;

		for (unsigned k = 0; k < nctp; k++)
			if (num(OMCI_ME_VOIP_VOICE_CTP, ctps[k], 1) == users[i])
				used = 1;
		if (used)
			continue;
		SAY("no port\n");
		user(users[i], NIL);
	}
	return OMCLI_OK;
}
