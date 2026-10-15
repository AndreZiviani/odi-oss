/*
 * Entry point: wire the tabs, load everything the page renders from, and start
 * the status poll.
 *
 * The UI renders from /api/schema, so a new config key appears without touching
 * any of this.
 */

import { $, el, fail, get, wireTheme } from './dom.js';
import { S, EDITS } from './state.js';
import { renderStatus } from './status.js';
import { renderAll } from './config.js';
import { save, refreshSaveBar, renderIdentitySwitch } from './save.js';
import { renderFirmware, wireFirmware, renderTrial } from './firmware.js';
import { renderServices } from './services.js';
import { renderMeBrowser } from './mebrowser.js';
import { wireRestore } from './restore.js';
import { renderL2 } from './l2.js';
import { renderTools } from './tools.js';
import { renderHostlink, wireHostlink } from './hostlink.js';

/*
 * Four sections, each with subtabs, and the location hash naming the pair:
 * #config/vlan, #system/logs. A bare #config lands on the first subtab.
 *
 * Both levels are ARIA tablists: arrow keys move between tabs and activate
 * them, Home and End jump to the ends, and only the selected tab is in the
 * tab order, so Tab goes from the tab row straight into its panel.
 */
const TABS = ['status', 'config', 'omci', 'system'];
/* Old tab names, so a bookmark from before the regrouping still lands. */
const OLD = {
  advanced: 'config/stock', services: 'omci/services', mib: 'omci/mib',
  tools: 'system/logs', admin: 'system/access', firmware: 'system/firmware',
};

/* What a view needs the first time it is shown. The OMCI and firmware views
   read the device, so they load on sight rather than at boot: seven forks of
   ~35 ms each is not something to spend before the status page has painted. */
const ON_SHOW = {
  'omci/services': () => renderServices(),
  'omci/mib': () => renderMeBrowser(),
  'system/firmware': () => renderFirmware(),
  'system/access': () => renderSshKeys(),
  'system/logs': () => renderTools(),
};

function subsOf(tab) {
  return [...document.querySelectorAll(`#p-${tab} .subtabs [role=tab]`)].filter((b) => !b.hidden);
}

function select(list, on) {
  for (const b of list) {
    const me = b === on;
    b.setAttribute('aria-selected', me ? 'true' : 'false');
    b.tabIndex = me ? 0 : -1;
    const panel = document.getElementById(b.getAttribute('aria-controls'));
    if (panel) panel.hidden = !me;
  }
}

function route() {
  let h = location.hash.replace(/^#/, '');
  if (OLD[h]) h = OLD[h];
  let [tab, sub] = h.split('/');
  if (!TABS.includes(tab)) tab = 'status';
  const top = [...document.querySelectorAll('.tabs [role=tab]')];
  select(top, top.find((b) => b.dataset.tab === tab));

  const subs = subsOf(tab);
  let view = tab;
  if (subs.length) {
    const on = subs.find((b) => b.dataset.sub === sub) || subs[0];
    select(subs, on);
    view = tab + '/' + on.dataset.sub;
  }
  /* The default-password banner is loud on the two views where it is about
     what you are looking at, and a small chip in the header everywhere else. */
  VIEW = view;
  showAuth();
  if (ON_SHOW[view]) ON_SHOW[view]();
}

let VIEW = 'status';
function showAuth() {
  const loud = VIEW === 'status' || VIEW === 'system/access';
  const on = !!(S.FW && S.FW.defaultauth);
  $('#defaultauth').hidden = !on || !loud;
  $('#authchip').hidden = !on || loud;
}

function go(view) {
  if (location.hash === '#' + view) route(); else location.hash = view;
}

/* Arrow keys, Home and End, on either level. */
function wireTablist(list, key) {
  list.addEventListener('keydown', (e) => {
    const tabs = [...list.querySelectorAll('[role=tab]')].filter((b) => !b.hidden);
    const i = tabs.indexOf(document.activeElement);
    if (i < 0) return;
    const j = e.key === 'ArrowRight' ? (i + 1) % tabs.length
      : e.key === 'ArrowLeft' ? (i - 1 + tabs.length) % tabs.length
      : e.key === 'Home' ? 0 : e.key === 'End' ? tabs.length - 1 : -1;
    if (j < 0) return;
    e.preventDefault();
    tabs[j].focus();
    tabs[j].click();
  });
  list.addEventListener('click', (e) => {
    const b = e.target.closest && e.target.closest('[role=tab]');
    if (b) key(b);
  });
}

wireTablist($('.tabs'), (b) => go(b.dataset.tab));
for (const list of document.querySelectorAll('.subtabs')) {
  const tab = list.closest('.panel').id.slice(2);
  wireTablist(list, (b) => go(tab + '/' + b.dataset.sub));
}
window.addEventListener('hashchange', route);
route();

$('#filter').oninput = () => renderAll();
$('#save').onclick = save;
$('#services-reload').onclick = () => renderServices(true);
$('#pw-save').onclick = savePassword;
$('#sshkey-add').onclick = addSshKey;
$('#reset-go').onclick = resetConfig;
wireRestore();
wireTheme();
wireFirmware();
wireHostlink();
/* On demand, not on the poll: it is another diag fork and most visits to the
   status page do not need it. Both firmwares answer it: the stock diag, and
   odi-oss diag from its own L2 table readback, under the same command. */
$('#l2-load').onclick = renderL2;
$('#gopassword').onclick = () => { go('system/access'); $('#pw-pass').focus(); };
$('#discard').onclick = () => { EDITS.clear(); renderAll(); refreshSaveBar(); $('#saveout').textContent = ''; };

/*
 * Say so while the built-in credential is in force. The fallback exists so a
 * freshly flashed stick is reachable at all; leaving it in place is a
 * different decision, and one the operator has to be able to see they are
 * making.
 */
async function checkAuth() {
  try {
    const fw = await get('/api/firmware');
    S.FW = fw;
  } catch (e) { /* the banner is advisory; a failed read must not blank the page */ }
  showAuth();
  renderTrial();
}

/*
 * Change the credential this page authenticates with, creating the file if it
 * is not there. That last part is the point: the state this fixes is the state
 * of every factory-reset stick, and a credential that can only be set from a
 * shell is one that stays at its default.
 */
async function savePassword() {
  const out = $('#pw-out');
  const user = $('#pw-user').value.trim();
  const pass = $('#pw-pass').value;

  out.textContent = '';
  try {
    const r = await fetch('/api/password', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ user, password: pass }).toString(),
    });
    const j = await r.json();
    if (!j.ok) { out.append(el('div', 'bad', j.error || 'refused')); return; }
    out.append(el('div', 'good', 'Saved to /etc/config/confd.auth.'));
    /* The browser still holds the OLD credential and will replay it on the
       next request, which now answers 401. Saying nothing here turns a
       successful change into what looks like a broken page. */
    out.append(el('div', null,
      'Your browser is still using the old one. Sign out, then sign back in as '
      + user + '.'));
    $('#pw-pass').value = '';
    await checkAuth();
  } catch (e) { out.append(el('div', 'bad', String(e.message || e))); }
}

/*
 * SSH public keys for the device login. The list is the file as it is, so a
 * key added over ssh by hand shows up here too, and a delete names the line
 * number the server reported rather than re-sending the key text.
 */
async function renderSshKeys() {
  const box = $('#sshkeys');
  box.textContent = '';
  try {
    const j = await get('/api/sshkeys');
    if (!j.keys.length) { box.append(el('p', 'hint', 'No keys yet: SSH accepts the root password only.')); return; }
    const t = el('ul', 'keylist');
    for (const k of j.keys) {
      const li = el('li');
      const parts = k.line.split(/\s+/);
      const blob = parts[1] || '';
      li.append(el('span', 'k-type', parts[0] || ''));
      li.append(el('span', 'k-blob mono', blob.length > 24 ? blob.slice(0, 12) + '\u2026' + blob.slice(-8) : blob));
      li.append(el('span', 'k-comment', parts.slice(2).join(' ')));
      const del = el('button', 'danger-quiet', 'Remove');
      del.type = 'button';
      del.setAttribute('aria-label', 'Remove key ' + (parts.slice(2).join(' ') || blob.slice(-8)));
      del.onclick = () => delSshKey(k.i);
      li.append(del);
      t.append(li);
    }
    box.append(t);
  } catch (e) { box.append(el('div', 'bad', String(e.message || e))); }
}

async function addSshKey() {
  const out = $('#sshkey-out');
  const key = $('#sshkey-new').value.trim();
  out.textContent = '';
  try {
    const r = await fetch('/api/sshkeys', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ key }).toString(),
    });
    const j = await r.json();
    if (!j.ok) { out.append(el('div', 'bad', j.error || 'refused')); return; }
    $('#sshkey-new').value = '';
    out.append(el('div', 'good', 'Added. dropbear reads the file on every login; nothing to restart.'));
    await renderSshKeys();
  } catch (e) { out.append(el('div', 'bad', String(e.message || e))); }
}

async function delSshKey(i) {
  const out = $('#sshkey-out');
  out.textContent = '';
  try {
    const r = await fetch('/api/sshkeys', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ delete: String(i) }).toString(),
    });
    const j = await r.json();
    if (!j.ok) { out.append(el('div', 'bad', j.error || 'refused')); return; }
    await renderSshKeys();
  } catch (e) { out.append(el('div', 'bad', String(e.message || e))); }
}

/*
 * Reset the service config, backup first.
 *
 * The backup is fetched as a blob and handed to the browser BEFORE the
 * destructive call, and the reset does not run if that fails. A link the
 * operator is told to click first is not a precondition -- it is a hope, and
 * the whole reason this device has bricked sticks in the field is resets done
 * without one.
 */
async function resetConfig() {
  const out = $('#reset-out');
  const btn = $('#reset-go');

  out.textContent = '';
  if (!confirm('Reset the service configuration to the image defaults?\n\n'
      + 'The keys in /etc/config_default.xml go back to their defaults: the four '
      + 'LOID keys are emptied, and DEVICE_TYPE, DUAL_MGMT_MODE and the three '
      + 'OMCI_CUSTOM masks (read only by the stock firmware) are reset. The '
      + 'management IP, the VLAN, the PLOAM password, the serial number and the '
      + 'MAC keys are NOT touched.\n\nA backup downloads first.')) return;

  btn.disabled = true;
  try {
    out.append(el('div', null, 'Downloading a backup…'));
    const r = await fetch('/api/backup', { cache: 'no-store' });
    if (!r.ok) throw new Error('backup failed: HTTP ' + r.status);
    const blob = await r.blob();
    if (!blob.size) throw new Error('the backup came back empty');

    /* Name it for the stick and the day, so a folder of these is still
       readable in six months. */
    const stamp = new Date().toISOString().slice(0, 10);
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `odi-config-${location.hostname}-${stamp}.xml`;
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 30000);
    out.append(el('div', 'good', `Backup saved (${blob.size} bytes).`));

    const p = await fetch('/api/reset', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'store=cs&_confirm=reset',
    });
    const j = await p.json();
    if (j.output) out.append(el('pre', null, j.output.trim()));
    if (!j.ok) { out.append(el('div', 'bad', j.error || 'the reset reported a failure')); return; }
    S.VALUES = await get('/api/values');
    renderAll();
    /* Of the reset keys, only the LOID ones are read here, by omcid, and
       they apply the way every omcid key does. */
    out.append(el('div', 'warn', 'Written. The LOID keys take effect when OMCI is '
      + 'applied (Apply now, on the Config tab save bar) or at the next reboot; the '
      + 'rest are read only by the stock firmware.'));
  } catch (e) {
    out.append(el('div', 'bad', String(e.message || e)));
    out.append(el('div', null, 'Nothing was reset.'));
  } finally {
    btn.disabled = false;
  }
}

async function refresh() {
  try {
    const st = await get('/api/status');
    if (st.error) throw new Error(st.error);
    renderStatus(st.raw);
  } catch (e) { fail(e); }
}

(async function init() {
  try {
    let metaRows, consRows, baseRows, featRows, settRows;
    [S.SCHEMA, S.VALUES, metaRows, consRows, S.DEFAULTS, baseRows, featRows, settRows, S.FW] =
      await Promise.all([
        get('/api/schema'), get('/api/values'), get('/api/meta'),
        get('/api/consumers'), get('/api/defaults'), get('/api/baseline'),
        get('/api/features'), get('/api/settings').catch(() => []),
        get('/api/firmware').catch(() => ({})),
      ]);
    for (const m of metaRows) S.META[m.name] = m;
    for (const r of settRows) S.SETTINGS[r.name] = r;
    for (const c of consRows) S.CONS[c.name] = c;
    for (const b of baseRows) S.BASELINE[b.name] = b.value;
    for (const f of featRows) (S.FEATURES[f.mask] ||= []).push(f);
    renderAll();
    renderIdentitySwitch();
  } catch (e) { fail(e); }
  await checkAuth();
  /* The host-link banner: files read by confd, no fork. */
  await renderHostlink();
  await refresh();
  /* A scrape is three forks of ~35 ms each on a ~300 BogoMIPS core, so this is
     deliberately unhurried. */
  setInterval(refresh, 15000);
})();
