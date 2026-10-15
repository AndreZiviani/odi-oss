/*
 * The save bar, the write, and applying it.
 *
 * Nothing is applied implicitly by the daemon: a write changes the store and
 * the response says what applying it takes. On an image with a settings
 * table that is one or more of three actions, and the page runs or offers
 * each one according to its class:
 *
 *   LIVE                  network, and omci for the VLAN keys: applied straight
 *                         after the save (omcid rebuilds in place on SIGHUP)
 *   SERVICE RESTART       syslog, ntp: the daemon restarts straight after the save
 *   INTERRUPTS INTERNET   omci: offered as "Apply now", behind a confirmation
 *                         (omcid re-registers the ONU)
 *   REBOOT                reboot: offered as "Reboot now", behind a confirmation
 *
 * Without the table (a stock-based image) it is the old choice between
 * restarting omci_app and a reboot.
 */

import { $, el, get } from './dom.js';
import {
  S, EDITS, CLASS_LABEL, CLASS_RANK, costBadge, imageAware, settingOf,
  IDENTITY_SWITCH, identitySwitchOn, vlanRisk,
} from './state.js';
import { invalidEdits } from './validate.js';
import { renderAll, markPending } from './config.js';

/* config.js raises this whenever a field changes; it does not import this
   module, so the dependency runs one way only. */
document.addEventListener('edits-changed', () => refreshSaveBar());

/* The keys that are written together with another one: LOID_OLD follows
   LOID, because the OLD value wins whenever the two differ, and saving LOID
   alone would change nothing the OLT sees. */
function withPairs(pairs) {
  const out = [...pairs];
  for (const [name, row] of Object.entries(S.SETTINGS)) {
    if (row.pair && EDITS.has(row.pair) && !EDITS.has(name)) out.push([name, EDITS.get(row.pair)]);
  }
  return out;
}

function refreshSaveBar() {
  const bar = $('#savebar');
  const n = EDITS.size;
  bar.hidden = n === 0;
  $('#savecount').textContent = n === 1 ? '1 change' : n + ' changes';
  /* Disabled rather than failing on click: the field is already marked, and a
     Save button that does nothing is worse than one that says it cannot. */
  const bad = invalidEdits();
  $('#save').disabled = bad.length > 0;
  $('#save').title = bad.length
    ? bad.map(([k, w]) => `${k}: ${w}`).join('\n') : '';
  const identity = [...EDITS.keys()].filter(
    (k) => (S.SCHEMA.find((r) => r.name === k) || {}).writable === 'identity');
  $('#confirmwrap').hidden = identity.length === 0;
  $('#confirmwhat').textContent = identity.join(', ');
  /* What saving these will cost, before the click rather than after it:
     one badge per class, most expensive first, each with how many of the
     pending changes carry it -- "1 interrupts internet, 2 live". */
  const byClass = {};
  for (const k of EDITS.keys()) {
    /* An identity key the switch keeps from the OLT costs nothing now. */
    const c = reportedNow(k) ? (settingOf(k) || {}).apply : null;
    if (c) byClass[c] = (byClass[c] || 0) + 1;
  }
  const host = $('#saveclasses');
  host.textContent = '';
  for (const c of Object.keys(byClass).sort((a, b) => CLASS_RANK[b] - CLASS_RANK[a])) {
    host.append(costBadge(c, byClass[c] + ' ' + (CLASS_LABEL[c] || c).toLowerCase()));
  }
  markPending();
}

function encode(pairs) {
  return pairs.map(([k, v]) =>
    encodeURIComponent(k) + '=' + encodeURIComponent(v)).join('&');
}

const form = (body) => ({
  method: 'POST',
  headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
  body: new URLSearchParams(body).toString(),
});

async function save() {
  const out = $('#saveout');
  out.textContent = '';
  const edited = [...EDITS.entries()];
  if (!edited.length) return;
  const pairs = withPairs(edited);

  if (!$('#confirmwrap').hidden && !$('#confirm').checked) {
    out.append(el('div', 'bad', 'Identity keys need the confirmation ticked.'));
    return;
  }

  /* The red marking on a field used to be the only consequence of an invalid
     value: save() never consulted it and POSTed anyway. The daemon rejects
     these too, but saying so here means the whole batch is not sent to find
     out. */
  const bad = invalidEdits();
  if (bad.length) {
    for (const [name, why] of bad) {
      out.append(el('div', 'bad', `${name}: ${why}`));
    }
    return;
  }
  if ($('#confirm').checked) pairs.push(['_confirm', 'identity']);

  /* The VLAN keys apply the moment they are saved, so this is the last
     point to say that a value can stop the traffic. */
  if (imageAware()) {
    const risk = vlanRisk(S.VALUES, { ...S.VALUES, ...Object.fromEntries(pairs) });
    if (risk && !confirmClass('live', risk + ' Continue?')) {
      out.append(el('div', 'warn', 'Not saved.'));
      return;
    }
  }

  $('#save').disabled = true;
  try {
    const r = await fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: encode(pairs),
    });
    if (r.status === 401) { location.href = '/'; return; }
    const res = await r.json();
    const written = [];

    for (const item of res.results || []) {
      const line = el('div', item.ok ? 'good' : 'bad');
      line.textContent = item.ok
        ? `${item.name} = ${item.value}`
        : `${item.name}: ${item.error}`;
      out.append(line);
      if (item.ok) { EDITS.delete(item.name); written.push(item.name); }
    }

    S.VALUES = await get('/api/values');
    renderAll();

    if (imageAware()) {
      await followUp(res, written, out);
      return;
    }

    /* A stock-based image: omci_app restarts, or the stick reboots. */
    if (res.apply === 'restart:omci') {
      const b = el('button', 'apply', 'Apply now: restarts OMCI, about 6 s, no reboot');
      b.onclick = (ev) => doApply(ev, 'omci');
      out.append(b);
    } else if (res.apply === 'reboot') {
      /* Two different statements, and the daemon distinguishes them: a key
         known to be read only at boot, versus a key nothing in the image was
         seen reading at all. */
      out.append(el('div', 'warn', res.untraced
        ? 'Nothing in the image was seen reading these keys, so assume a ' +
          'reboot is needed. Nothing here reboots the stick for you.'
        : 'These keys are read at boot, so a reboot is needed for them to ' +
          'take effect. Nothing here reboots the stick for you.'));
    }
  } catch (e) {
    out.append(el('div', 'bad', String(e.message || e)));
  } finally {
    $('#save').disabled = false;
    refreshSaveBar();
  }
}

/*
 * After a save on an image that says what each key costs: run the live
 * action, offer the other two. `written` is what actually stuck.
 */
async function followUp(res, written, out) {
  const needs = res.needs || [];

  if (res.stock) {
    out.append(el('div', 'hint', 'Some of these keys are read only by the stock '
      + 'firmware in the other slot; nothing on this image changes.'));
  }
  if (needs.includes('network')) {
    const moves = written.includes('LAN_IP_ADDR') || written.includes('LAN_SUBNET');
    if (!moves || confirmClass('live', 'The management address changes now, and this '
        + `page with it, to ${S.VALUES.LAN_IP_ADDR}. Continue?`)) {
      await applyNetwork(out, moves);
    } else {
      out.append(el('div', 'warn', 'Saved, not applied: the address changes at the next '
        + 'reboot, or when you apply it.'));
      out.append(applyButton('live', 'Apply the addresses now', () => applyNetwork(out, true)));
    }
  }
  for (const [what, daemon] of [['syslog', 'syslogd'], ['ntp', 'ntpd']]) {
    if (needs.includes(what)) await applyService(out, what, daemon);
  }
  /* A key omcid reports only while the identity switch is on changes
     nothing on the line while it is off: omcid finds no difference, so
     there is nothing to apply and nothing drops. */
  const omciKeys = written.filter((k) => (settingOf(k) || {}).action === 'omci');
  const unreported = omciKeys.filter((k) => !reportedNow(k));
  if (needs.includes('omci') && unreported.length && unreported.length === omciKeys.length) {
    out.append(el('div', 'hint', 'Saved. Not reported to the OLT while the identity switch '
      + 'is off, so nothing changes on the line and there is nothing to apply.'));
  } else if (needs.includes('omci')) {
    if (res.interrupts && omciKeys.some((k) => reportedNow(k)
        && (settingOf(k) || {}).apply === 'internet')) {
      out.append(el('div', 'warn', 'Saved. These keys take effect when omcid '
        + 're-registers the ONU and the OLT provisions it again.'));
      out.append(applyButton('internet', 'Apply now', () => applyOmci(out, true)));
    } else {
      /* Only VLAN keys: omcid rebuilds the connections in place, nothing drops. */
      await applyOmci(out, false);
    }
  }
  if (needs.includes('reboot')) {
    out.append(el('div', 'warn', 'Saved. These keys are read only at boot.'));
    out.append(applyButton('reboot', 'Reboot', () => reboot(out)));
  }
}

/* Whether omcid reports a key now: the identity ones only with the switch on. */
function reportedNow(name) {
  const set = settingOf(name);
  return !(set && /omci-identity\.on/.test(set.reader)) || identitySwitchOn();
}

/* A button that says its class and asks before REBOOT or INTERRUPTS INTERNET. */
function applyButton(cls, text, run) {
  const b = el('button', 'apply cls-' + cls);
  b.type = 'button';
  b.append(document.createTextNode(text + ' '), costBadge(cls));
  b.onclick = async () => {
    if (cls === 'internet' && !confirmClass(cls, 'This takes the fibre service down: the '
        + 'ONU is deactivated, omcid clears its MIB, and the ONU ranges again and '
        + 'waits for the OLT to provision it. About ten seconds, then however long '
        + 'the OLT takes (typically under a minute). Continue?')) return;
    if (cls === 'reboot' && !confirmClass(cls, rebootWarning())) return;
    b.disabled = true;
    try { await run(); } finally { b.remove(); }
  };
  return b;
}

function confirmClass(cls, text) {
  return confirm(`${'\u25AE'.repeat(CLASS_RANK[cls] || 0)} ${CLASS_LABEL[cls] || cls}\n\n${text}`);
}

/* Which slot a reboot comes back on, which is the whole question on a trial. */
function rebootWarning() {
  const env = S.FW.env || {};
  const trial = env.sw_active !== undefined && env.sw_commit !== undefined
    && env.sw_active !== env.sw_commit;
  return 'The stick reboots and the fibre service drops for about two minutes.'
    + (trial ? ` This is a trial of partition ${env.sw_active}: the reboot comes `
      + `back on partition ${env.sw_commit}, the committed one, not on this image.` : '')
    + ' Continue?';
}

async function applyNetwork(out, moves) {
  try {
    const r = await fetch('/api/apply', form({ what: 'network' }));
    const res = await r.json();
    if (res.output) out.append(el('pre', null, String(res.output).trim()));
    out.append(el('div', res.ok ? 'good' : 'bad', res.ok
      ? 'Addresses applied.' : 'Not applied: ' + (res.error || 'see above')));
    if (res.ok && moves && S.VALUES.LAN_IP_ADDR && S.VALUES.LAN_IP_ADDR !== location.hostname) {
      const a = el('a', 'fwbtn', `Continue on http://${S.VALUES.LAN_IP_ADDR}/`);
      a.href = `http://${S.VALUES.LAN_IP_ADDR}/`;
      out.append(a);
    }
  } catch (e) {
    /* The address moving under the request is one way this ends. */
    out.append(el('div', 'warn', 'No answer: if the address changed, the page '
      + `is now at http://${S.VALUES.LAN_IP_ADDR}/.`));
  }
}

/* SERVICE RESTART: apply.sh kills the daemon, init respawns it with the new
   setting. Nothing to confirm, the fibre service is not touched. */
async function applyService(out, what, daemon) {
  try {
    const r = await fetch('/api/apply', form({ what }));
    const res = await r.json();
    if (res.output) out.append(el('pre', null, String(res.output).trim()));
    out.append(el('div', res.ok ? 'good' : 'bad', res.ok
      ? `${daemon} restarted with the new setting.`
      : `Saved, but ${daemon} was not restarted: ` + (res.error || 'see above')));
  } catch (e) {
    out.append(el('div', 'bad', String(e.message || e)));
  }
}

/* omcid rereads the store on SIGHUP (apply.sh omci sends it and reports what
   omcid did). `interrupts` says whether the keys re-register the ONU or only
   rebuild the connections in place. */
async function applyOmci(out, interrupts) {
  out.append(el('div', 'warn', interrupts ? 'Re-registering the ONU…'
    : 'Rebuilding the connections in place…'));
  try {
    const r = await fetch('/api/apply', form({ what: 'omci' }));
    const res = await r.json();
    if (res.output) out.append(el('pre', null, String(res.output).trim()));
    out.append(el('div', res.ok ? 'good' : 'bad', res.ok
      ? (interrupts ? 'Applied. The Status tab shows O5 and the Services tab the '
        + 'provisioned services once the OLT is done.'
        : 'Applied. The ONU stayed in O5.')
      : 'Apply failed: ' + (res.error || 'see above')));
  } catch (e) {
    out.append(el('div', 'bad', String(e.message || e)));
  }
}

async function reboot(out) {
  try {
    await fetch('/api/firmware', form({ action: 'reboot' }));
    out.append(el('div', 'warn', 'Rebooting. This page will stop responding.'));
  } catch (e) {
    out.append(el('div', 'bad', String(e.message || e)));
  }
}

/* The legacy apply, kept for a stock-based image. */
async function doApply(ev, what) {
  const out = $('#saveout');
  ev.target.disabled = true;
  ev.target.textContent = 'Restarting OMCI…';
  try {
    const r = await fetch('/api/apply', form({ what }));
    const res = await r.json();
    out.append(el('div', res.applied ? 'good' : 'bad',
      res.applied ? 'Applied: the OMCI daemon restarted and the ONU is re-registering.'
                  : 'Apply failed: ' + (res.error || 'unknown')));
  } catch (e) {
    out.append(el('div', 'bad', String(e.message || e)));
  } finally {
    ev.target.remove();
  }
}

/*
 * The ONU identity switch. omcid reports OMCI_SW_VER1/2, GPON_ONU_MODEL,
 * OMCC_VER, OMCI_VENDOR_PRODUCT_CODE and ONU_HW_VERSION only while
 * /etc/config/omci-identity.on exists: a stick commonly ships with the stock
 * values already stored in the XML ones, so honouring them by default would
 * change what the OLT sees. Toggling it is INTERRUPTS INTERNET, because
 * omcid re-registers the ONU to make the OLT read them again.
 */
function renderIdentitySwitch() {
  const host = $('#identity-switch');
  host.textContent = '';
  if (!imageAware()) { host.hidden = true; return; }
  host.hidden = false;
  const on = identitySwitchOn();
  const text = el('div', 'switch-text');
  text.append(el('strong', null, on ? 'Reporting the stored identity' : 'Reporting the image defaults'));
  text.append(el('p', 's-help', on
    ? 'The OLT is told the versions, model, hardware version, OMCC version and product code below. An empty key keeps the default.'
    : 'The OLT is told software version 0.0.0, the device id as model and hardware version, OMCC 128 and product code 15, whatever the keys below hold.'));
  text.append(el('p', 's-help', 'Some OLTs provision a service only for the identity they '
    + 'expect: one they do not expect can leave the ONU in O5 with no service. Switching '
    + 'interrupts internet while the ONU registers again.'));
  const b = el('button', null, on ? 'Report the defaults' : 'Report the stored identity');
  b.type = 'button';
  b.onclick = async () => {
    b.disabled = true;
    const out = $('#identity-out');
    out.textContent = '';
    try {
      const r = await fetch('/api/switch', form({ name: IDENTITY_SWITCH, on: on ? '0' : '1' }));
      const res = await r.json();
      if (!res.ok) { out.append(el('div', 'bad', res.error || 'refused')); return; }
      S.FW.switches = { ...(S.FW.switches || {}), [IDENTITY_SWITCH]: res.on };
      renderIdentitySwitch();
      renderAll();
      out.append(el('div', 'warn', 'Switched. It takes effect when OMCI is applied.'),
        applyButton('internet', 'Apply now', () => applyOmci(out, true)));
    } catch (e) {
      out.append(el('div', 'bad', String(e.message || e)));
    } finally { b.disabled = false; }
  };
  const acts = el('div', 'switch-acts');
  acts.append(b, costBadge('internet'));
  host.append(text, acts);
}

export { refreshSaveBar, save, doApply, withPairs, renderIdentitySwitch, applyButton, rebootWarning, followUp, reportedNow };
