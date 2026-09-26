/* The identity the line authenticates with, out of the jffs2 config store.
 *
 * `/etc/config` is a symlink to `/var/config`, mtd3 mounted jffs2, and the
 * store is two ordinary XML files -- not an opaque blob and not something that
 * needs `libmib.so.0`. The vendor's own
 * scripts read it by shelling out to `mib get`, which a freestanding daemon
 * cannot do and does not need to: the format is three elements deep.
 *
 *     <Config Name="ROOT">
 *       <Dir Name="MIB_TABLE">            <!-- HW_MIB_TABLE in the hs file -->
 *         <Value Name="KEY" Value="VALUE"/>
 *       </Dir>
 *     </Config>
 *
 * Which file a key is in comes from xmlconfig's own 102-row descriptor table,
 * not from guessing: ten keys are HS and the rest CS. The split is not what the
 * names suggest -- LOID, LOID_PASSWD and LOID_OLD are service config while
 * LOID_PASSWD_OLD alone is hardware identity.
 *
 * This does not parse XML. It looks for the exact byte sequence a Value element
 * starts with, which is all these files ever contain, and refuses anything it
 * does not recognise rather than guessing. A store written by something other
 * than xmlconfig would read as absent, which is the safe direction.
 */
#include "omcid.h"

/* Big enough for the whole store: isp1's is a few KB and the partition is
 * 240 KB with 204 KB free, so a file that does not fit here is not a config
 * store. Static, because there is no allocator. */
static char cfgbuf[16384];

/* The five entities xmlconfig carries, in two tables in its rodata beside the
 * `<Value Name="%s" Value=` writer format. So values in the store ARE escaped,
 * and a reader that does not undo it is wrong.
 *
 * It does not bite on GPON_SN, which is [A-Z0-9]. It bites on a PLOAM or LOID
 * password, which are the two fields that decide whether a line authenticates
 * -- and it bites silently, because nothing can tell a wrong password from a
 * wrong password. The first version of this file had that bug.
 *
 * Longest first is not needed here (no entity is a prefix of another) but the
 * table is ordered that way anyway so adding one cannot introduce the problem.
 */
static const struct { const char *ent; int len; char ch; } xml_ent[] = {
	{ "&quot;", 6, '"' },
	{ "&apos;", 6, '\'' },
	{ "&amp;",  5, '&'  },
	{ "&lt;",   4, '<'  },
	{ "&gt;",   4, '>'  },
};

#define XML_ENT_N ((int)(sizeof xml_ent / sizeof xml_ent[0]))

/* Decode `n` bytes into `out`, NUL-terminated. Returns the DECODED length,
 * which is what the caller reports -- an entity is five or six bytes on disk
 * and one in the value, and the difference is exactly what tells a present-
 * but-empty key from a missing one. */
static int xml_unescape(const char *in, int n, char *out, int max)
{
	int i = 0, o = 0;

	while (i < n) {
		int k;

		for (k = 0; k < XML_ENT_N; k++) {
			int l = xml_ent[k].len;
			int m;

			if (in[i] != '&' || i + l > n)
				continue;
			for (m = 1; m < l; m++)
				if (in[i + m] != xml_ent[k].ent[m])
					break;
			if (m == l)
				break;
		}
		if (k < XML_ENT_N) {
			if (o < max - 1)
				out[o] = xml_ent[k].ch;
			i += xml_ent[k].len;
		} else {
			if (o < max - 1)
				out[o] = in[i];
			i++;
		}
		o++;
	}
	out[o < max - 1 ? o : max - 1] = 0;
	return o;
}

/* The inverse, for the writer. Returns the encoded length, or -1 if it would
 * not fit -- silently truncating a password is the one failure mode this must
 * not have. */
static int xml_escape(const char *in, char *out, int max)
{
	int o = 0;

	for (int i = 0; in[i]; i++) {
		int k;

		for (k = 0; k < XML_ENT_N; k++)
			if (in[i] == xml_ent[k].ch)
				break;
		if (k < XML_ENT_N) {
			if (o + xml_ent[k].len > max)
				return -1;
			for (int m = 0; m < xml_ent[k].len; m++)
				out[o + m] = xml_ent[k].ent[m];
			o += xml_ent[k].len;
		} else {
			if (o + 1 > max)
				return -1;
			out[o++] = in[i];
		}
	}
	return o;
}

/* One key out of one file. Returns the value's length, or -1 when the file or
 * the key is not there -- which is NOT the same as a key present and empty, and
 * the caller has to tell them apart: an empty LOID means "do not send -l" and a
 * missing one means the store was never written. */
int cfg_get(const char *path, const char *key, char *out, int max)
{
	long fd = sys_open(path, 0);
	int n = 0, klen = str_len(key);

	if (fd < 0)
		return -1;
	for (;;) {
		long r = sys_read((int)fd, cfgbuf + n, sizeof cfgbuf - 1 - n);

		if (r <= 0)
			break;
		n += (int)r;
		if (n >= (int)sizeof cfgbuf - 1)
			break;
	}
	sys_close((int)fd);
	cfgbuf[n] = 0;

	for (int i = 0; i + 13 < n; i++) {
		int j;

		/* `<Value Name="` -- 13 bytes, and the only place a key name
		 * appears. Matching on the key alone would also hit the Dir
		 * element and any description text. */
		if (cfgbuf[i] != '<' || cfgbuf[i + 1] != 'V'
		    || cfgbuf[i + 2] != 'a' || cfgbuf[i + 3] != 'l'
		    || cfgbuf[i + 4] != 'u' || cfgbuf[i + 5] != 'e'
		    || cfgbuf[i + 6] != ' ' || cfgbuf[i + 7] != 'N'
		    || cfgbuf[i + 8] != 'a' || cfgbuf[i + 9] != 'm'
		    || cfgbuf[i + 10] != 'e' || cfgbuf[i + 11] != '='
		    || cfgbuf[i + 12] != '"')
			continue;
		j = i + 13;
		/* The key must match to its closing quote, so LOID does not
		 * answer for LOID_OLD. */
		for (int k = 0; k < klen; k++)
			if (j + k >= n || cfgbuf[j + k] != key[k])
				goto next;
		if (j + klen >= n || cfgbuf[j + klen] != '"')
			goto next;
		j += klen + 1;
		/* ` Value="` -- the attribute order xmlconfig writes. Anything
		 * else is not a file it wrote. */
		if (j + 8 > n || cfgbuf[j] != ' ' || cfgbuf[j + 1] != 'V'
		    || cfgbuf[j + 2] != 'a' || cfgbuf[j + 3] != 'l'
		    || cfgbuf[j + 4] != 'u' || cfgbuf[j + 5] != 'e'
		    || cfgbuf[j + 6] != '=' || cfgbuf[j + 7] != '"')
			goto next;
		j += 8;
		{
			int end = j;

			while (end < n && cfgbuf[end] != '"')
				end++;
			if (end >= n)
				return -1;       /* unterminated */
			return xml_unescape(cfgbuf + j, end - j, out, max);
		}
next:
		;
	}
	return -1;
}

/* Replace one key's value, or insert the key, and write the file back.
 *
 * Deliberately NOT what `flash set` does. That runs `xmlconfig -s` to update
 * libmib's shared state and then `xmlconfig -of` to dump the WHOLE state over
 * the file -- so every key libmib holds is rewritten, and the store has 102 of
 * them against the six we understand. This edits one attribute in place and
 * leaves every byte around it alone, which cannot lose a key we do not model.
 *
 * The other divergence is atomicity. `flash set` overwrites lastgood.xml
 * directly -- the script has a ${LASTGOOD_FILE}_bak name but only ever removes
 * it -- and on jffs2 a power cut mid-write is how a stick comes back without
 * its serial. This writes a sibling temp file and renames over the target. The
 * bytes that end up in the file are identical either way; only the failure mode
 * differs, so this is not a behaviour change.
 *
 * Returns 0, or -1 and writes nothing. `dir` is the element the key belongs
 * under when it has to be inserted -- "MIB_TABLE" or "HW_MIB_TABLE" -- and
 * getting it wrong creates a duplicate the vendor's own reader will not see.
 */
int cfg_set(const char *path, const char *dir, const char *key, const char *value)
{
	static char outbuf[sizeof cfgbuf + 256];
	char esc[192];
	char tmp[128];
	int n, elen, o = 0, klen = str_len(key), dlen = str_len(dir);
	int at = -1, vstart = -1, vend = -1, dirend = -1;
	long fd;

	elen = xml_escape(value, esc, (int)sizeof esc);
	if (elen < 0)
		return -1;                       /* would truncate: refuse */
	esc[elen] = 0;

	fd = sys_open(path, 0);
	if (fd < 0)
		return -1;
	n = 0;
	for (;;) {
		long r = sys_read((int)fd, cfgbuf + n, sizeof cfgbuf - 1 - n);

		if (r <= 0)
			break;
		n += (int)r;
		if (n >= (int)sizeof cfgbuf - 1)
			break;
	}
	sys_close((int)fd);
	cfgbuf[n] = 0;

	/* Find the key's value span, and -- in case it is absent -- the end of
	 * the Dir it belongs in. Both in one pass over a file of a few KB. */
	for (int i = 0; i + 5 < n; i++) {
		if (cfgbuf[i] == '<' && cfgbuf[i + 1] == '/'
		    && cfgbuf[i + 2] == 'D' && cfgbuf[i + 3] == 'i'
		    && cfgbuf[i + 4] == 'r' && dirend < 0 && at < 0) {
			/* The first </Dir> after the Dir whose name matched. */
			if (dirend == -2)
				dirend = i;
			continue;
		}
		if (cfgbuf[i] == '<' && cfgbuf[i + 1] == 'D'
		    && cfgbuf[i + 2] == 'i' && cfgbuf[i + 3] == 'r') {
			int j = i + 4;

			while (j + 6 < n && cfgbuf[j] != '"')
				j++;
			j++;
			{
				int m = 0;

				while (m < dlen && j + m < n
				       && cfgbuf[j + m] == dir[m])
					m++;
				if (m == dlen && j + m < n && cfgbuf[j + m] == '"')
					dirend = -2;     /* inside our Dir now */
			}
			continue;
		}
		if (i + 13 >= n || cfgbuf[i] != '<' || cfgbuf[i + 1] != 'V'
		    || cfgbuf[i + 2] != 'a' || cfgbuf[i + 3] != 'l'
		    || cfgbuf[i + 4] != 'u' || cfgbuf[i + 5] != 'e'
		    || cfgbuf[i + 6] != ' ' || cfgbuf[i + 7] != 'N'
		    || cfgbuf[i + 8] != 'a' || cfgbuf[i + 9] != 'm'
		    || cfgbuf[i + 10] != 'e' || cfgbuf[i + 11] != '='
		    || cfgbuf[i + 12] != '"')
			continue;
		{
			int j = i + 13, k;

			for (k = 0; k < klen; k++)
				if (j + k >= n || cfgbuf[j + k] != key[k])
					break;
			if (k != klen || j + klen >= n || cfgbuf[j + klen] != '"')
				continue;
			j += klen + 1;
			if (j + 8 > n || cfgbuf[j] != ' ' || cfgbuf[j + 1] != 'V'
			    || cfgbuf[j + 2] != 'a' || cfgbuf[j + 3] != 'l'
			    || cfgbuf[j + 4] != 'u' || cfgbuf[j + 5] != 'e'
			    || cfgbuf[j + 6] != '=' || cfgbuf[j + 7] != '"')
				continue;
			at = i;
			vstart = j + 8;
			vend = vstart;
			while (vend < n && cfgbuf[vend] != '"')
				vend++;
			if (vend >= n)
				return -1;
			break;
		}
	}
	/* dirend is -2 while inside the matching Dir and a byte offset once its
	 * closing tag is seen. The loop above covers every byte of the file, so
	 * a -2 here means the Dir was opened and never closed -- a malformed
	 * store, which is refused rather than repaired. */
	if (at < 0 && dirend < 0)
		return -1;                       /* no such Dir: refuse */

	if (at >= 0) {
		for (int i = 0; i < vstart; i++)
			outbuf[o++] = cfgbuf[i];
		for (int i = 0; i < elen; i++)
			outbuf[o++] = esc[i];
		for (int i = vend; i < n; i++)
			outbuf[o++] = cfgbuf[i];
	} else {
		static const char pre[] = "\t\t<Value Name=\"";
		static const char mid[] = "\" Value=\"";
		static const char post[] = "\"/>\n";

		for (int i = 0; i < dirend; i++)
			outbuf[o++] = cfgbuf[i];
		for (int i = 0; i < (int)sizeof pre - 1; i++)
			outbuf[o++] = pre[i];
		for (int i = 0; i < klen; i++)
			outbuf[o++] = key[i];
		for (int i = 0; i < (int)sizeof mid - 1; i++)
			outbuf[o++] = mid[i];
		for (int i = 0; i < elen; i++)
			outbuf[o++] = esc[i];
		for (int i = 0; i < (int)sizeof post - 1; i++)
			outbuf[o++] = post[i];
		for (int i = dirend; i < n; i++)
			outbuf[o++] = cfgbuf[i];
	}

	/* Same directory, so the rename is within one filesystem. */
	str_copy(tmp, path, (int)sizeof tmp - 5);
	{
		int t = str_len(tmp);

		tmp[t] = '.'; tmp[t + 1] = 'n'; tmp[t + 2] = 'e';
		tmp[t + 3] = 'w'; tmp[t + 4] = 0;
	}
	fd = sys_create(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	for (int done = 0; done < o; ) {
		long w = sys_write((int)fd, outbuf + done, (unsigned long)(o - done));

		if (w <= 0) {
			sys_close((int)fd);
			sys_unlink(tmp);
			return -1;
		}
		done += (int)w;
	}
	sys_close((int)fd);
	if (sys_rename(tmp, path) < 0) {
		sys_unlink(tmp);
		return -1;
	}
	return 0;
}

#define CFG_CS "/var/config/lastgood.xml"
const char *const CFG_CS_PATH = CFG_CS;
#define CFG_HS "/var/config/lastgood_hs.xml"

struct onu_identity ident;
struct onu_vlan_cfg vlanCfg;

/* `omci_app`'s command line is built by /etc/runomci.sh, and the LOID rule
 * there is not "use LOID":
 *
 *     if (LOID_OLD == LOID)  use LOID;  else  use LOID_OLD;
 *
 * which is to say the OLD value wins whenever the two disagree -- including
 * when LOID_OLD is empty and LOID is not, in which case the vendor passes no
 * -l at all and the new value is silently ignored. The same for the password,
 * whose OLD half lives in the hs file while its current half is in cs, so a
 * `flash default cs` leaves them disagreeing and the old password in force.
 *
 * Reproduced exactly. It is surprising enough that doing the sensible thing
 * instead would make us behave differently from the stick beside us. */
static void pick_old_wins(const char *cur, int curLen,
			  const char *old, int oldLen, char *out, int max)
{
	const char *src = cur;
	int len = curLen;

	if (oldLen < 0 || curLen < 0 || !str_eq(cur, old)) {
		src = old;
		len = oldLen;
	}
	if (len < 0) {
		out[0] = 0;
		return;
	}
	str_copy(out, src, max);
}

/* The paths are an argument so the test can point this at fixtures. Note which
 * file each key comes from: LOID, LOID_PASSWD and LOID_OLD are cs, while
 * LOID_PASSWD_OLD is hs. That is xmlconfig's split, not a typo. */
/* One key from whichever of the two files carries it, the file xmlconfig
 * assigns it to first.
 *
 * xmlconfig's split is where a stick that has only ever run the vendor
 * firmware keeps a key, but it is not where every stick keeps it: isp1 holds
 * GPON_PLOAM_PASSWD and LOID_PASSWD_OLD in the cs file, and a reader that
 * looked in hs alone reported both as missing. flash get and rcS already look
 * in cs; this is the same answer from the other side. A key in neither file is
 * still -1, absent. */
static int cfg_get_either(const char *first, const char *second,
			  const char *key, char *out, int max)
{
	int n = cfg_get(first, key, out, max);

	return n >= 0 ? n : cfg_get(second, key, out, max);
}

void cfg_load_identity_from(const char *cs, const char *hs)
{
	char loid[64], loidOld[64], pwd[64], pwdOld[64];
	int a, b;

	ident.snLen   = cfg_get_either(hs, cs, "GPON_SN", ident.sn, sizeof ident.sn);
	ident.ploamLen = cfg_get_either(hs, cs, "GPON_PLOAM_PASSWD",
					ident.ploam, sizeof ident.ploam);
	a = cfg_get_either(cs, hs, "LOID", loid, sizeof loid);
	b = cfg_get_either(cs, hs, "LOID_OLD", loidOld, sizeof loidOld);
	pick_old_wins(loid, a, loidOld, b, ident.loid, sizeof ident.loid);
	a = cfg_get_either(cs, hs, "LOID_PASSWD", pwd, sizeof pwd);
	b = cfg_get_either(hs, cs, "LOID_PASSWD_OLD", pwdOld, sizeof pwdOld);
	pick_old_wins(pwd, a, pwdOld, b, ident.loidPwd, sizeof ident.loidPwd);
	ident.loaded = 1;
}

void cfg_load_identity(void)
{
	cfg_load_identity_from(CFG_CS, CFG_HS);
}

/* -------------------------------------------------------- the manual VLAN
 *
 * The VLAN a service carries is NOT in class 171. Both our sticks keep it in
 * the config store, and the extended-VLAN table the OLT sets carries the "no
 * VID" sentinel (4096) in every treatment field:
 *
 *     VLAN_CFG_TYPE      1
 *     VLAN_MANU_MODE     1
 *     VLAN_MANU_TAG_VID  10 on isp2, 11 on isp1
 *     VLAN_MANU_TAG_PRI  0
 *
 * and each stick's live bridge connection adds exactly its own VID. So the ONU
 * is tagging locally -- `OMCI_GenTrafficRule` reaches a feature plugin twice,
 * and isp2's OMCI_CUSTOM_BDP selects `cf_sfu_report_veip` -- and reproducing
 * the service is a matter of reading four keys rather than of rebuilding the
 * generator.
 *
 * `mode` is reported separately from the VID so a caller can tell "manual mode
 * is off" from "manual mode is on and the VID happens to be 0".
 */
static int cfg_num(const char *path, const char *key, int missing)
{
	char buf[16];
	int n = cfg_get(path, key, buf, sizeof buf);
	int v = 0, i = 0;

	if (n <= 0)
		return missing;
	for (; buf[i] >= '0' && buf[i] <= '9'; i++)
		v = v * 10 + (buf[i] - '0');
	return i ? v : missing;
}

/* The manual tag is applied only in the one combination the vendor stack
 * applies it in. /etc/runomci.sh passes `-iot_vt 1 -iot_vm 1 -iot_vid V
 * -iot_pri P` only when VLAN_CFG_TYPE is 1, VLAN_MANU_MODE is 1, and both the
 * VID and the priority are present; every other combination hands omci_app the
 * 65535/255 sentinels, which is no manual tag at all. ISP1 and ISP2 both run
 * 1/1 with a VID and a priority, so the applied path never triggers on
 * either. */
#define VLAN_CFG_TYPE_MANUAL  1
#define VLAN_MANU_MODE_TAG    1

void cfg_load_vlan_from(const char *cs)
{
	int pri;

	vlanCfg.mode = cfg_num(cs, "VLAN_MANU_MODE", 0);
	vlanCfg.type = cfg_num(cs, "VLAN_CFG_TYPE", 0);
	vlanCfg.vid  = cfg_num(cs, "VLAN_MANU_TAG_VID", -1);
	pri          = cfg_num(cs, "VLAN_MANU_TAG_PRI", -1);
	vlanCfg.pri  = pri < 0 ? 0 : pri;
	vlanCfg.manual = vlanCfg.type == VLAN_CFG_TYPE_MANUAL
		&& vlanCfg.mode == VLAN_MANU_MODE_TAG
		&& vlanCfg.vid >= 0 && pri >= 0;
	vlanCfg.loaded = 1;
}

/* The VID the connection builder tags with, or -1 for none. The one place the
 * gate above is applied, so the builder and `omcli bridge` cannot disagree. */
int cfg_manual_vid(void)
{
	if (!vlanCfg.loaded)
		cfg_load_vlan();
	return vlanCfg.manual ? vlanCfg.vid : -1;
}

void cfg_load_vlan(void)
{
	cfg_load_vlan_from(CFG_CS);
}

void cfg_show_vlan(void)
{
	if (!vlanCfg.loaded)
		cfg_load_vlan();
	out_fmt("vlan mode   %d (manual tagging %s)\n", (long)vlanCfg.mode,
		vlanCfg.mode ? "on" : "off");
	out_fmt("vlan type   %d\n", (long)vlanCfg.type);
	if (vlanCfg.vid < 0)
		out("vlan vid    (not in the store)\n");
	else
		out_fmt("vlan vid    %d\n", (long)vlanCfg.vid);
	out_fmt("vlan pri    %d\n", (long)vlanCfg.pri);
	if (vlanCfg.manual)
		out_fmt("manual tag  applied: vid %d pri %d\n",
			(long)vlanCfg.vid, (long)vlanCfg.pri);
	else
		out("manual tag  not applied (needs type 1, mode 1, a vid and a pri)\n");
}

/* What is safe to print. The serial is on the label and the OLT ranges on it,
 * so it is not a secret; the PLOAM password and the LOID password are the
 * line's credentials and this is the same discipline the frame dump already
 * applies to classes 148, 157, 340 and 134 -- say whether it is set and how
 * long, never what it is. A log gets pasted. */
void cfg_show_identity(void)
{
	if (!ident.loaded)
		cfg_load_identity();
	out_fmt("serial      %s\n", ident.snLen >= 0 ? ident.sn : "(not in the store)");
	out_fmt("ploam       %s", ident.ploamLen > 0 ? "set, " : "");
	if (ident.ploamLen > 0)
		out_fmt("%d bytes (withheld)", (long)ident.ploamLen);
	else
		out(ident.ploamLen == 0 ? "empty" : "(not in the store)");
	out_char('\n');
	out_fmt("loid        %s\n", ident.loid[0] ? ident.loid : "(none -- nothing would be sent)");
	out_fmt("loid pw     %s\n", ident.loidPwd[0] ? "set (withheld)" : "(none)");
	if (!report.loaded)
		cfg_load_report();
	out_fmt("report      %s (" CFG_REPORT_SWITCH ")\n",
		report.on ? "on" : "off -- the keys below are stored, not reported");
	for (int i = 0; i < 2; i++) {
		out_fmt("sw ver %d    ", (long)i);
		if (report.swVerLen[i] > 0)
			out_fmt("%s%s\n", report.swVer[i], report.on ? "" : " (not reported)");
		else
			out("(not in the store: " REPORT_DEFAULT_SW_VER ")\n");
	}
	if (report.modelLen > 0)
		out_fmt("onu model   %s%s\n", report.model, report.on ? "" : " (not reported)");
	else
		out("onu model   (not in the store: the device id)\n");
	if (report.omccVer >= 0)
		out_fmt("omcc ver    %d%s\n", (long)report.omccVer, report.on ? "" : " (not reported)");
	else
		out("omcc ver    (not in the store: 128)\n");
	if (report.productCode >= 0)
		out_fmt("prod code   %d%s\n", (long)report.productCode, report.on ? "" : " (not reported)");
	else
		out("prod code   (not in the store: the captured default)\n");
}

/* ------------------------------------------------ what the OLT is told
 *
 * Five keys describe this ONU to the OLT, in two managed entities:
 *
 *     OMCI_SW_VER1, OMCI_SW_VER2   software image (class 7) instance 0 and 1,
 *                                  attribute 1 Version, 14 bytes
 *     GPON_ONU_MODEL               ONU2-G (class 257) attribute 1, Equipment
 *                                  id, 20 bytes -- "IGD" on the vendor stack,
 *                                  the same string both sticks store
 *     OMCC_VER                     ONU2-G attribute 2, one byte
 *     OMCI_VENDOR_PRODUCT_CODE     ONU2-G attribute 3, two bytes
 *
 * An empty or absent key keeps what omcid has always answered: "0.0.0", the
 * device id, 0x80 and the captured product code.
 *
 * Reported only while CFG_REPORT_SWITCH exists, and that is not caution for
 * its own sake. Both our sticks already carry all five, written by the vendor
 * firmware (it rewrites OMCI_SW_VER1/2 itself as it boots and flashes), so
 * honouring them unconditionally would change what both OLTs see today --
 * the software version from "0.0.0" to the vendor string and the equipment id
 * from the device id to "IGD" -- on an image whose point is that nothing
 * changes by default. With the switch absent the values are loaded and shown
 * by `omcli ident`, and nothing else. */
struct onu_report report;

static int cfg_num_str(const char *s, int n, int max)
{
	int v = 0;

	if (n <= 0)
		return -1;
	for (int i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9')
			return -1;
		v = v * 10 + (s[i] - '0');
		if (v > max)
			return -1;
	}
	return v;
}

void cfg_load_report_from(const char *cs, const char *hs, const char *sw)
{
	char buf[32];
	int n;

	for (int i = 0; i < 2; i++) {
		n = cfg_get_either(cs, hs, i ? "OMCI_SW_VER2" : "OMCI_SW_VER1",
				   report.swVer[i], sizeof report.swVer[i]);
		report.swVerLen[i] = n;
	}
	report.modelLen = cfg_get_either(hs, cs, "GPON_ONU_MODEL",
					 report.model, sizeof report.model);
	n = cfg_get_either(cs, hs, "OMCC_VER", buf, sizeof buf);
	report.omccVer = cfg_num_str(buf, n, REPORT_OMCC_VER_MAX);
	n = cfg_get_either(cs, hs, "OMCI_VENDOR_PRODUCT_CODE", buf, sizeof buf);
	report.productCode = cfg_num_str(buf, n, REPORT_PRODUCT_CODE_MAX);
	{
		long fd = sys_open(sw, 0);

		report.on = fd >= 0;
		if (fd >= 0)
			sys_close((int)fd);
	}
	report.loaded = 1;
}

void cfg_load_report(void)
{
	cfg_load_report_from(CFG_CS, CFG_HS, CFG_REPORT_SWITCH);
}

/* The software-image version to answer for instance `inst`: the key, or the
 * "0.0.0" omcid has always answered. */
const char *report_sw_ver(uint16_t inst)
{
	if (!report.loaded)
		cfg_load_report();
	if (report.on && inst < 2 && report.swVerLen[inst] > 0)
		return report.swVer[inst];
	return REPORT_DEFAULT_SW_VER;
}
