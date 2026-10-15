/*
 * The Config rows: one per key, rendered from the schema.
 *
 * Adding a key to the device adds a row here with no code change -- the schema
 * is data, and meta.tsv is what turns a name into a labelled, explained,
 * range-checked control. GROUPS below only decides which subtab a key lands
 * on; a key it does not name still appears, under the subtab its schema
 * section maps to, or under Other.
 */

import { $, el } from './dom.js';
import {
  S, EDITS, provenance, applyOf, dependsUnmet, gatesOthers, dropIgnoredEdits, decodeMask,
  CLASS_LABEL, CLASS_MEANS, costBadge, imageAware, settingOf, identitySwitchOn,
} from './state.js';
import { hexAscii, validateInput } from './validate.js';

/*
 * The subtabs, grouped by what someone changing the line thinks about rather
 * than by the schema section a key happens to live in: the schema puts the
 * LOID in `other` and the UNI MAC in `hardware`, which is where nobody looks
 * for them.
 */
const GROUPS = {
  line: ['GPON_SN', 'GPON_PLOAM_PASSWD', 'LOID', 'LOID_PASSWD', 'LOID_OLD', 'LOID_PASSWD_OLD'],
  vlan: ['VLAN_CFG_TYPE', 'VLAN_MANU_MODE', 'VLAN_MANU_TAG_VID', 'VLAN_MANU_TAG_PRI'],
  identity: ['OMCI_VENDOR_PRODUCT_CODE', 'GPON_ONU_MODEL', 'ONU_HW_VERSION', 'OMCI_SW_VER1', 'OMCI_SW_VER2', 'OMCC_VER',
    'OLT_SW_DOWNLOAD', 'OMCI_ACCEPT_REBOOT', 'OMCI_UNKNOWN_ME_OK', 'OMCI_FIXED_ME'],
  network: ['LAN_IP_ADDR', 'LAN_SUBNET', 'LAN_ENABLE_IP2', 'LAN_IP_ADDR2', 'LAN_SUBNET2', 'ELAN_MAC_ADDR'],
  services: ['SYSLOG_SERVER', 'NTP_SERVER'],
};
/* Keys with a control of their own on their subtab, not a schema row there:
   HOST_AUTONEG is never written by Save or Restore (hostlink.js runs a trial
   instead), and its schema row is writable=never, so the daemon refuses it
   too. Kept off the Stock keys subtab as well: this image reads it. */
const OWN_CONTROL = ['HOST_AUTONEG'];
/* A key GROUPS does not name, by its schema section. */
const BY_SECTION = { gpon: 'line', vlan: 'vlan', omci: 'identity', lan: 'network', hardware: 'network' };

function groupOf(row) {
  for (const [g, keys] of Object.entries(GROUPS)) if (keys.includes(row.name)) return g;
  return BY_SECTION[row.section] || 'other';
}

/*
 * One line of help, and the rest behind a disclosure. The first sentence of
 * the meta.tsv help is what the key IS; everything after it, and the
 * settings.tsv note (why it costs what it does), is the reason, and belongs
 * one click away rather than in a paragraph on every row.
 *
 * The two files were written separately and some rows said the same thing in
 * both, which the page used to print twice. A sentence already said is not
 * said again.
 */
const sentences = (t) => String(t || '').split(/(?<=[.!?])\s+(?=[A-Z`(0-9])/).filter(Boolean);
const norm = (s) => s.toLowerCase().replace(/[^a-z0-9]+/g, ' ').trim();

function splitHelp(help, note) {
  const h = sentences(help);
  const seen = new Set(h.map(norm));
  const n = sentences(note).filter((s) => !seen.has(norm(s)));
  return { first: h[0] || '', rest: h.slice(1).join(' '), note: n.join(' ') };
}

function renderValue(row, raw, readonly = false) {
  const box = el('div', 's-field');
  const meta = S.META[row.name] || {};
  const set = settingOf(row.name);

  /* What to PUT IN THE FIELD: the pending edit if there is one, otherwise the
     device value. `raw` stays the comparison baseline throughout.

     Keeping edits in a Map outside the DOM is what lets a tab switch or a
     filter keystroke rebuild the rows without losing them -- but only if the
     rebuild reads the Map back. A value the user cannot see is a value they
     cannot check. */
  /* An absent key the image gives a value of its own (settings.tsv `unset`)
     shows that value, as what is in effect. It is still the baseline only as
     absent: choosing it again stores nothing. */
  const fallback = (raw === undefined || raw === '') && set && set.unset ? set.unset : undefined;
  const shown = EDITS.has(row.name) ? EDITS.get(row.name) : (fallback !== undefined ? fallback : raw);

  /* Never-writable keys get no control at all. The refusal is enforced in the
     daemon twice over, but not offering the field is the honest presentation.
     Nor do the stock keys, on an image that says which keys it reads, nor a
     key that is written together with another one (LOID_OLD follows LOID). */
  /* A key the firmware ignores right now shows no value either: whatever it
     holds does nothing, and a stale or odd one only invites a fix nobody
     needs. The stored value is under Details, for when the mode changes. */
  if (dependsUnmet(row) && !readonly) {
    box.append(el('span', 's-ro', 'Not used in this mode'));
    return box;
  }
  if (row.writable === 'never' || readonly || (set && set.pair)) {
    box.append(el('span', 's-ro mono', shown === undefined ? '(not set)'
      : shown === '' ? '(empty)' : shown));
    return box;
  }

  let input;
  if (meta.options) {
    input = el('select');
    for (const pair of meta.options.split('|')) {
      const eq = pair.indexOf('=');
      const o = el('option', null, pair.slice(eq + 1));
      o.value = pair.slice(0, eq);
      input.append(o);
    }
    /* A value the device holds that is not in the option list must still be
       selectable, or opening the page would silently propose changing it.
       A key with NO value shows what the image does without it (settings.tsv
       `unset`, required for every key with options); only a key the image
       does not describe can still read "(not set)". */
    if (raw === undefined || raw === '') {
      if (fallback === undefined) {
        const o = el('option', null, '(not set)');
        o.value = '';
        input.append(o);
      }
    } else if (![...input.options].some((o) => o.value === raw)) {
      const o = el('option', null, raw + ' (current, not a listed value)');
      o.value = raw;
      input.append(o);
    }
    if (shown !== raw && ![...input.options].some((o) => o.value === shown)) {
      const o = el('option', null, shown);
      o.value = shown;
      input.append(o);
    }
    /* Unset is the empty option: a select given undefined shows nothing. */
    input.value = shown === undefined ? '' : shown;
  } else {
    input = el('input');
    input.type = 'text';
    input.value = shown === undefined ? '' : shown;
    input.spellcheck = false;
    input.autocomplete = 'off';
    if (row.type === 'int') input.inputMode = 'numeric';
    if (row.type !== 'string' && row.type !== 'hostport' && row.type !== 'ascii14') input.classList.add('mono');
    if (meta.range) input.placeholder = meta.range.replace('-', '–');
  }
  input.id = 'f-' + row.name;
  input.dataset.name = row.name;
  /* Anything else on this row that has to follow the field as it is typed
     registers here, rather than adding a second listener: one handler means no
     ordering to get wrong, and the DOM stub in scripts/web-check.mjs only has
     to model the one. */
  let follow = null;
  input.oninput = input.onchange = () => {
    const v = input.value;
    const same = v === raw || (fallback !== undefined && v === fallback);
    if (same) EDITS.delete(row.name); else EDITS.set(row.name, v);
    input.classList.toggle('changed', !same);
    validateInput(row, input);
    if (follow) follow(v);
    /* A key others depend on locks or unlocks them: redraw, and keep the
       focus on the control that was changed. */
    if (gatesOthers(row.name)) {
      dropIgnoredEdits();
      renderAll();
      const again = $('#f-' + row.name);
      if (again) again.focus();
    }
    /* An event, not an import of save.js: config renders the fields and save
       owns the bar, and importing each other would make that a cycle. */
    document.dispatchEvent(new Event('edits-changed'));
  };
  input.classList.toggle('changed', EDITS.has(row.name));
  validateInput(row, input);
  box.append(input);

  /* The OMCI_CUSTOM_* masks decode to the plugins the image will load, on
     every keystroke: the point is to see what a value DOES before saving it.
     Upstream has had an open issue since 2022 asking what OMCI_CUSTOM_RDP=4
     means, and the answer is one line this device could always have printed. */
  if (decodeMask(row.name, '0')) {
    const bits = el('div', 's-aside bits mono');
    const paint = (v) => {
      const d = decodeMask(row.name, v);
      bits.textContent = '';
      if (String(v ?? '').trim() === '') { bits.append(el('div', null, 'not set')); return; }
      if (!d) { bits.append(el('div', null, 'not a number')); return; }
      if (!d.n) { bits.append(el('div', null, 'no features enabled')); return; }
      for (const b of d.known)
        bits.append(el('div', null, '0x' + b.bit.toString(16) + '  ' + b.names.join(', ')));
      for (const b of d.unknown)
        bits.append(el('div', 'unknownbit',
          '0x' + b.toString(16) + '  no plugin in this image, does nothing'));
    };
    paint(shown);
    follow = paint;
    box.append(bits);
  }

  /* PLOAM and friends store the hex of an ASCII string; show the readable form
     beside the field it is stored in. */
  if (row.type === 'hexascii') {
    const txt = hexAscii(raw);
    box.append(el('div', 's-aside', txt === null ? 'Not printable ASCII' : 'Reads as “' + txt + '”'));
  }
  return box;
}

/* One setting: label and help on the left, the control and its cost on the right. */
function renderRow(row, readonly) {
  const meta = S.META[row.name] || {};
  const set = settingOf(row.name);
  const prov = provenance(row, S.VALUES[row.name]);
  const wrap = el('div', 'setting');
  wrap.dataset.name = row.name;

  const left = el('div', 's-label');
  const name = el('label', 's-name', meta.label || row.name);
  const unmet = dependsUnmet(row);
  if (!(row.writable === 'never' || readonly || (set && set.pair) || unmet)) name.htmlFor = 'f-' + row.name;
  const title = el('div', 's-title');
  title.append(name, el('span', 's-key mono', row.name));
  left.append(title);

  const h = splitHelp(meta.help, set && set.note);
  if (h.first) left.append(el('p', 's-help', h.first));

  /* The things that are wrong NOW stay visible: a value the firmware is
     currently ignoring is not a detail. */
  if (row.name === 'LAN_IP_ADDR' && S.FW.lanip_override) {
    left.append(el('p', 's-warn', 'Ignored while /etc/config/lan-ip exists: that file sets the address, with a /24 mask.'));
  }
  if (set && /omci-identity\.on/.test(set.reader) && !identitySwitchOn()) {
    left.append(el('p', 's-warn', 'Not reported to the OLT: the identity switch is off, so '
      + 'saving it changes nothing on the line and does not drop the service.'));
  }
  if (unmet) {
    left.append(el('p', 's-warn', 'Not editable now: the firmware ignores this key unless '
      + unmet.join(' and ') + '. Change that first.'));
  }

  const more = [];
  if (h.rest) more.push(['', h.rest]);
  if (h.note) more.push(['', h.note]);
  if (set && set.pair) more.push(['Written with', set.pair + ': edit that one.']);
  if (meta.options) {
    more.push(['Accepts', meta.options.split('|').map((p) => p.slice(0, p.indexOf('='))).join(', ')]);
  }
  /* Who reads the key. Useful even where the timing is unknown, and it is
     what the apply class was derived from. */
  const rd = imageAware() ? (set ? set.reader : '') : (S.CONS[row.name] || {}).readers;
  if (rd) more.push(['Read by', rd.split(',').map((x) => x.trim()).join(', ')]);
  if (set && set.unset && (S.VALUES[row.name] === undefined || S.VALUES[row.name] === '')) {
    more.push(['Stored', `not set (the image uses ${set.unset})`]);
  }
  if (unmet && S.VALUES[row.name] !== undefined) {
    more.push(['Stored', `${S.VALUES[row.name] === '' ? '(empty)' : S.VALUES[row.name]} (used again once ${unmet.join(' and ')})`]);
  }
  if (prov && prov.was !== undefined) more.push(['Was', `${prov.was === '' ? '(empty)' : prov.was} (${prov.from})`]);
  if (more.length) {
    const d = el('details', 's-more');
    d.append(el('summary', null, 'Details'));
    const dl = el('dl');
    for (const [k, v] of more) {
      if (k) dl.append(el('dt', null, k));
      dl.append(el('dd', k ? null : 'wide', v));
    }
    d.append(dl);
    left.append(d);
  }
  wrap.append(left);

  const right = el('div', 's-control');
  right.append(renderValue(row, S.VALUES[row.name], readonly));
  const tags = el('div', 's-tags');
  if (imageAware()) {
    /* The apply class this image gives the key: what saving it costs. */
    if (set) tags.append(costBadge(set.apply));
  } else {
    const ap = applyOf(row);
    if (ap === 'restart:omci') tags.append(costBadge('internet', 'Restarts OMCI'));
    if (ap === 'reboot') tags.append(costBadge('reboot'));
  }
  if (row.writable === 'never') tags.append(el('span', 'tag', 'Never written'));
  if (row.writable === 'identity' && !readonly) {
    const t = el('span', 'tag identity', 'Identity');
    t.title = 'The line authenticates on this key. Saving it asks you to confirm.';
    tags.append(t);
  }
  if (prov && !unmet) {
    const t = el('span', 'tag ' + prov.cls, prov.label === 'image default' ? 'Default' : 'Changed');
    if (prov.was !== undefined) t.title = `${prov.from}: ${prov.was === '' ? '(empty)' : prov.was}`;
    tags.append(t);
  }
  if (tags.children.length) right.append(tags);
  wrap.append(right);
  return wrap;
}

/*
 * `sections` groups the rows under their schema section headings; the
 * editable subtabs are already grouped, so only the stock list uses it.
 */
function renderConfig(hostSel, rows, filter, sections = false, readonly = false) {
  const host = $(hostSel);
  host.textContent = '';
  /* A read-only list is for scanning, so it is drawn dense. */
  host.classList.toggle('compact', readonly);
  const f = (filter || '').toLowerCase();
  const bySection = {};

  for (const row of rows) {
    if (f && !(row.name.toLowerCase().includes(f) || row.section.includes(f)
               || ((S.META[row.name] || {}).label || '').toLowerCase().includes(f))) continue;
    (bySection[sections ? row.section : ''] ||= []).push(row);
  }
  for (const name of Object.keys(bySection).sort()) {
    if (sections) host.append(el('h3', 'sec', name));
    for (const row of bySection[name]) host.append(renderRow(row, readonly));
  }
  if (!Object.keys(bySection).length && f) host.append(el('p', 'hint', 'No key matches “' + filter + '”.'));
}

/* The cost legend, once, above the subtabs' content. */
function renderLegend() {
  const host = $('#legend');
  host.textContent = '';
  if (!imageAware()) return;
  for (const c of ['live', 'restart', 'internet', 'reboot']) {
    const s = el('span', 'legend-item');
    s.append(costBadge(c), document.createTextNode(CLASS_MEANS[c]));
    host.append(s);
  }
}

/* Subtab labels carry the number of pending edits on that subtab, so a change
   typed two subtabs ago is not forgotten. */
function markPending() {
  const counts = {};
  for (const k of EDITS.keys()) {
    const row = S.SCHEMA.find((r) => r.name === k);
    if (!row) continue;
    const g = imageAware() && !settingOf(k) ? 'stock' : groupOf(row);
    counts[g] = (counts[g] || 0) + 1;
  }
  for (const b of document.querySelectorAll('#p-config [role=tab]')) {
    let n = b.querySelector('.pending');
    const c = counts[b.dataset.sub] || 0;
    if (!c) { if (n) n.remove(); continue; }
    if (!n) { n = el('span', 'pending'); b.append(n); }
    n.textContent = String(c);
    n.title = c + ' unsaved';
  }
}

function renderAll() {
  const filter = $('#filter').value;
  renderLegend();

  const editable = (imageAware()
    ? S.SCHEMA.filter((r) => settingOf(r.name))
    : S.SCHEMA.filter((r) => r.common === 'yes' && r.section !== 'accounts'))
    .filter((r) => !OWN_CONTROL.includes(r.name));
  const groups = {};
  for (const r of editable) (groups[groupOf(r)] ||= []).push(r);
  /* In the order GROUPS lists them: the order someone reads a line in. */
  const rank = (r) => { const i = (GROUPS[groupOf(r)] || []).indexOf(r.name); return i < 0 ? 99 : i; };
  for (const g of Object.values(groups)) g.sort((a, b) => rank(a) - rank(b));
  for (const g of ['line', 'vlan', 'identity', 'network', 'services', 'other']) {
    renderConfig('#set-' + g, groups[g] || [], '');
  }
  $('#st-config-other').hidden = !(groups.other || []).length;

  if (imageAware()) {
    /* The keys this image reads, editable with their apply class; everything
       else is the stock firmware own store, shown read-only on its own subtab
       and kept whole in backups and restores. */
    renderConfig('#sections', S.SCHEMA.filter((r) => !settingOf(r.name) && !OWN_CONTROL.includes(r.name)),
      filter, true, true);
    /* The device-login keys are the stock firmware accounts; this image
       logs in as root with SSH keys, so the section is not offered. */
    $('#device-login').hidden = true;
    $('#advanced-deck').textContent = 'Keys only the stock firmware reads, shown read-only. '
      + 'They stay because the config partition is shared with the other slot.';
  } else {
    /* No settings table: every key offered, as before it existed. The account
       keys are the stock SSH login, so they live on System > Access. */
    renderConfig('#accounts', S.SCHEMA.filter((r) => r.common === 'yes' && r.section === 'accounts'), '');
    renderConfig('#sections', S.SCHEMA, filter, true);
    $('#st-config-stock').textContent = 'All keys';
    $('#advanced-deck').textContent = 'Every key the device stores, editable.';
  }
  markPending();
}

export { renderValue, renderConfig, renderAll, renderRow, splitHelp, groupOf, markPending, GROUPS, OWN_CONTROL };
