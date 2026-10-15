/*
 * Reading the OMCI MIB, and turning what omcicli prints into something the two
 * ME pages can render.
 *
 * One parser, two pages. `mebrowser.js` shows the blocks as they came;
 * `services.js` picks a handful apart and says what they mean. Both go through
 * here so there is exactly one place that knows the output shape -- and that
 * shape is not consistent even within one firmware. ME 84 prints `EntityID`,
 * ME 171 prints `EntityId`, and a table's rows are bare lines with no key at
 * all. The parser is deliberately permissive about all of it: an attribute it
 * does not recognise still reaches the page as text, because a MIB dump this
 * daemon cannot label is far more useful than one it drops.
 */

import { get } from './dom.js';

/*
 * omcicli frames each table with rows of X and separates instances with rows of
 * =. Both are decoration; neither carries data.
 */
const isBanner = (l) => /^X{3,}\s*$/.test(l);
const isRule = (l) => /^={3,}\s*$/.test(l);

/*
 * Parse one `omcicli mib get` dump.
 *
 * Returns { name, instances: [ { id, attrs: [[k, v]], groups: [{label, attrs}] } ] }.
 *
 * `name` is the table name omcicli itself printed, and the pages display it
 * rather than the label they asked for. That is the check that keeps a wrong
 * class id from being quietly mislabelled: ask for 84 and the card says
 * whatever came back, so a mismatch is visible instead of assumed away.
 */
function parseOmci(text) {
  const lines = String(text || '').split('\n').map((l) => l.replace(/\r$/, ''));
  const instances = [];
  let name = '';
  let cur = null;
  let group = null;

  const flush = () => {
    if (cur && (cur.attrs.length || cur.groups.length)) instances.push(cur);
    cur = null;
    group = null;
  };

  let rows = null;
  let odi = false;

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];

    /*
       odi-oss omcid prints its own shape for every class it has no vendor
       renderer for: a `<class> <Name> <instance>` header per row, then one
       `    Attr   <hex bytes>` line per attribute, and a `N rows` count at
       the end. Read off src/omci/respond/show.c (cli_row). Values are raw
       bytes, so they are turned into what the vendor dump would have
       printed: a number as 0x.., a string as its text.
    */
    const hdr = /^(\d+) (\S+) (\d+)(\s+\(row truncated\))?\s*$/.exec(line);
    if (hdr) {
      flush();
      odi = true;
      name = hdr[2];
      cur = { id: '0x' + Number(hdr[3]).toString(16).padStart(4, '0'), attrs: [], groups: [], truncated: !!hdr[4] };
      continue;
    }
    const cnt = /^(\d+) rows?\s*$/.exec(line);
    if (cnt) { flush(); rows = Number(cnt[1]); continue; }
    if (odi && cur) {
      const a = /^ {4}(\S+)\s+([0-9a-fA-F]+)\s*$/.exec(line);
      if (a) { (group ? group.attrs : cur.attrs).push([a[1], hexValue(a[2])]); continue; }
      if (/^ {4}(\S+)\s*$/.test(line)) { group = { label: line.trim(), attrs: [] }; cur.groups.push(group); continue; }
      const g = /^ {6}\s*(\d+)\s+(.*)$/.exec(line);
      if (g && group) { group.attrs.push([g[1], g[2]]); continue; }
    }

    /* The table name sits between two banner rows, once, at the top. */
    if (isBanner(line)) {
      const next = lines[i + 1];
      if (next && !isBanner(next) && !isRule(next) && isBanner(lines[i + 2] || '')) {
        name = next.trim();
        i += 2;
      }
      continue;
    }
    if (isRule(line)) { flush(); continue; }

    const t = line.trim();
    if (!t) continue;
    if (!cur) { cur = { id: '', attrs: [], groups: [] }; group = null; }

    /*
       An INDENTED line continues the attribute above it rather than starting
       anything. Real dumps do this in at least two places: ME 131 breaks
       `ToDInfo` into tab-indented sub-lines that themselves contain colons,
       and ME 171 follows `DscpToPbitMapping:` with eight indented hex words
       and no colons at all. Treating those as new keys or new sub-tables
       produced eight empty headings and attached the instance's own remaining
       attributes to the last INDEX row.
    */
    if (/^\s/.test(line) && (group ? group.attrs.length : cur.attrs.length)) {
      const into = group ? group.attrs : cur.attrs;
      const last = into[into.length - 1];

      last[1] = last[1] ? last[1] + ' ' + t : t;
      continue;
    }

    const c = t.indexOf(':');
    if (c > 0) {
      const k = t.slice(0, c).trim();
      const v = t.slice(c + 1).trim();
      if (/^entity\s*id$/i.test(k)) cur.id = v;
      (group ? group.attrs : cur.attrs).push([k, v]);
    } else {
      /* A line with no colon opens a sub-table: `ReceivedFrameVlanTaggingOperTable`
         and then one `INDEX n` per row. */
      group = { label: t, attrs: [] };
      cur.groups.push(group);
    }
  }
  flush();
  return { name, instances, rows };
}

/* Raw attribute bytes from the odi-oss dump, as the vendor dump would print
   them: up to four bytes is a number; longer is a string if it reads as one
   (NUL padding dropped), and hex otherwise. */
function hexValue(hex) {
  if (hex.length <= 8) return '0x' + hex;
  const t = hex.replace(/(00)+$/, '');
  if (!t) return '';
  let out = '';
  for (let i = 0; i < t.length; i += 2) {
    const c = parseInt(t.slice(i, i + 2), 16);
    if (c < 0x20 || c > 0x7e) return '0x' + hex;
    out += String.fromCharCode(c);
  }
  return out;
}

/* Case-insensitive attribute lookup, top level only. */
function attr(inst, key) {
  const want = String(key).toLowerCase();
  for (const [k, v] of inst.attrs) if (k.toLowerCase() === want) return v;
  return null;
}

/*
 * Fetch one dump. Never throws for a device-side condition: an ONU that has not
 * registered answers with nothing at all, and "nothing at all" is a finding the
 * pages render, not an error they hide.
 */
async function fetchOmci(params) {
  const qs = new URLSearchParams(params).toString();
  try {
    const r = await get('/api/omci?' + qs);
    return {
      raw: r.raw || '',
      truncated: !!r.truncated,
      ok: r.ok !== false,
      error: r.error || '',
      ...parseOmci(r.raw || ''),
    };
  } catch (e) {
    return { raw: '', truncated: false, ok: false, error: String(e.message || e), name: '', instances: [] };
  }
}

/*
 * Whether a dump means "the MIB service did not answer" rather than "this table
 * is empty".
 *
 * Both failure modes were seen on our own sticks: `omcicli` either prints
 * nothing at all, or prints its registered-table listing instead of the table
 * you asked for. Rendering either as "the OLT has not created any" is the most
 * misleading thing this page could say -- it reads as a provisioning fault on a
 * line that is carrying traffic. Both were seen with the stock omci_app, which
 * stops servicing its request queue after a long run; omcid has not shown it,
 * but a daemon that is not running answers the same way.
 */
function unavailable(dump) {
  if (!dump.ok) return dump.error || 'the MIB could not be read';
  const raw = String(dump.raw || '');

  if (!raw.trim())
    return 'omcicli returned nothing: the OMCI daemon is not answering. '
         + 'Check that omcid is running (ps); starting it again is described '
         + 'in the image docs (TOOLS.md, Restarting a daemon).';
  if (/^no managed entity called /m.test(raw))
    return 'omcid does not know this table: ' + raw.trim();
  if (/^TableId\s*\[\d+\]\s*Name:/m.test(raw) && !dump.instances.length)
    return 'omcicli listed its tables instead of reading the one asked for, '
         + 'which is how the stock omci_app fails when it has stopped '
         + 'servicing its request queue.';
  return null;
}

/* One ME, by class id or table name. */
const fetchMe = (me, entity) =>
  fetchOmci(entity ? { cmd: 'me', me, entity } : { cmd: 'me', me });

export { parseOmci, attr, fetchOmci, fetchMe, unavailable };
