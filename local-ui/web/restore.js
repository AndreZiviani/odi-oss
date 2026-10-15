/*
 * Restore a configuration backup.
 *
 * Deliberately no new daemon route. The browser reads the file, parses it, and
 * replays it through /api/config -- the same path the Config tab writes
 * through, which already validates every value against the schema, refuses the
 * SerDes keys, demands _confirm=identity for the nine that the line
 * authenticates on, and reads every write back. A dedicated restore endpoint
 * taking a whole XML file would be a way around all of that, and the one key it
 * would let through unchecked is LAN_SDS_MODE -- which is the key that takes
 * ssh and this page with it.
 *
 * It also means a restore is reviewable. The file is diffed against what the
 * device holds and nothing is written until the differences have been shown.
 */

import { $, el } from './dom.js';
import { S } from './state.js';

/*
 * `flash all` prints the config store as <Value Name="X" Value="Y"/> nested in
 * <Dir> elements. Parsed with a regex rather than DOMParser on purpose: the
 * device emits table rows as sibling <Dir> blocks distinguished by an XML
 * COMMENT (<!--index=1-->), which no XML parser will hand back, and the
 * flat name=value view is all a restore needs.
 */
function parseBackup(text) {
  const out = new Map();
  const re = /<Value\s+Name="([^"]+)"\s+Value="([^"]*)"\s*\/>/g;
  let m;

  while ((m = re.exec(text))) {
    /* Later wins: `flash all` prints hs then cs, and a key present in both is
       the cs one the device actually serves from /api/values. */
    out.set(m[1], m[2]);
  }
  return out;
}

/* What the schema says we may write, and what it costs. */
function classify(backup) {
  const rows = new Map(S.SCHEMA.map((r) => [r.name, r]));
  const plan = { same: [], change: [], identity: [], refused: [], unknown: [] };

  for (const [name, value] of backup) {
    const row = rows.get(name);

    if (!row) { plan.unknown.push(name); continue; }
    if (row.writable === 'never') {
      /* Only report it as refused if it would actually have changed
         something -- a backup always contains these, and listing nine
         untouched keys as problems every time trains people to ignore the
         list. */
      if (S.VALUES[name] !== value) plan.refused.push(name);
      continue;
    }
    if (S.VALUES[name] === value) { plan.same.push(name); continue; }
    (row.writable === 'identity' ? plan.identity : plan.change).push([name, value]);
  }
  return plan;
}

const line = (cls, text) => el('div', cls, text);

function renderPlan(plan, host) {
  host.textContent = '';
  const n = plan.change.length + plan.identity.length;

  if (!n) {
    host.append(line('good', 'This backup matches what the stick already holds. '
      + 'Nothing to restore.'));
    return false;
  }

  host.append(line(null, `${plan.change.length} setting${plan.change.length === 1 ? '' : 's'} differ`
    + (plan.identity.length ? `, plus ${plan.identity.length} identity key${plan.identity.length === 1 ? '' : 's'}` : '')
    + `. ${plan.same.length} already match.`));

  const list = el('table');
  for (const [name, value] of [...plan.change, ...plan.identity]) {
    const tr = el('tr');
    const now = S.VALUES[name];

    tr.append(el('td', null, name));
    tr.append(el('td', 'mono', now === undefined || now === '' ? '(empty)' : now));
    tr.append(el('td', 'mono', value === '' ? '(empty)' : value));
    list.append(tr);
  }
  host.append(list);

  if (plan.refused.length)
    host.append(line('warn', 'Not restorable, and skipped: ' + plan.refused.join(', ')
      + '. A wrong SerDes mode costs every management path at once, so this '
      + 'daemon refuses those keys however they arrive.'));
  if (plan.unknown.length)
    host.append(line('warn', plan.unknown.length + ' key(s) in the file are not in '
      + "this image's schema and will be skipped."));
  return true;
}

/*
 * Apply in batches. /api/config is per-key -- a refused or invalid key does not
 * stop the others and each gets its own verdict -- so the batch size is only
 * about not building a request larger than the daemon will read.
 */
async function apply(pairs, withIdentity) {
  const results = [];

  for (let i = 0; i < pairs.length; i += 12) {
    const chunk = pairs.slice(i, i + 12);
    const body = new URLSearchParams(chunk);

    if (withIdentity) body.set('_confirm', 'identity');
    const r = await fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: body.toString(),
    });
    const j = await r.json();
    for (const res of j.results || []) results.push(res);
  }
  return results;
}

let PLAN = null;

function wireRestore() {
  const file = $('#restore-file');
  const out = $('#restore-out');
  const go = $('#restore-go');

  go.disabled = true;

  file.onchange = async () => {
    PLAN = null;
    go.disabled = true;
    out.textContent = '';
    const f = file.files && file.files[0];

    if (!f) return;
    const text = await f.text();
    const backup = parseBackup(text);

    if (!backup.size) {
      out.append(line('bad', 'No settings found in that file. A backup looks '
        + 'like <Value Name="..." Value="..."/> — it is what the Download '
        + 'button above produces.'));
      return;
    }
    PLAN = classify(backup);
    go.disabled = !renderPlan(PLAN, out);
  };

  go.onclick = async () => {
    if (!PLAN) return;
    const withIdentity = $('#restore-identity').checked;
    const pairs = [...PLAN.change, ...(withIdentity ? PLAN.identity : [])];

    if (!pairs.length) return;
    go.disabled = true;
    out.append(line(null, `Writing ${pairs.length} setting(s)…`));

    const results = await apply(pairs, withIdentity);
    const bad = results.filter((r) => !r.ok);

    out.append(line(bad.length ? 'warn' : 'good',
      `${results.length - bad.length} written, ${bad.length} refused.`));
    for (const r of bad) out.append(line('bad', `${r.name}: ${r.error}`));

    if (!withIdentity && PLAN.identity.length)
      out.append(line('warn', PLAN.identity.length + ' identity key(s) were left '
        + 'alone. Tick the box above to include them — only do that when '
        + 'restoring onto the stick the backup came from.'));

    out.append(line('warn', 'Written, not applied. Each restored key takes effect '
      + 'the way its class on the Config tab says: the addresses at once when '
      + 'you save them again there, the OMCI keys with Apply now, the rest at '
      + 'the next reboot. Keys only the stock firmware reads change nothing here.'));
  };
}

export { parseBackup, classify, wireRestore };
