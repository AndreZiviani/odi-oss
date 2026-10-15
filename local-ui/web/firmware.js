/*
 * The Firmware tab: which partition is which, and the one-shot trial boot.
 *
 * sw_tryactive boots a partition ONCE with the watchdog armed, so an image that
 * does not come up reverts itself. Committing it is a separate, deliberate act.
 */

import { $, el, fail, get, bytes } from './dom.js';
import { S, CLASS_LABEL, CLASS_RANK } from './state.js';

/* How often the page asks how a background write is going. */
const WRITE_POLL_MS = 3000;

/*
 * An image sitting in /tmp, if one has been uploaded this session. The device
 * is not asked -- a stale /tmp/img.tar from some previous attempt is exactly
 * what should NOT quietly become writable with one click.
 */
let UPLOADED = null;
let COMMIT_WHY = null;

/*
 * Upload. XHR rather than fetch, for the one thing fetch cannot do: report
 * progress while the body is going out. Three megabytes over a link this
 * device drives at its own pace is long enough that a button which simply
 * greys out looks broken.
 */
function uploadImage() {
  const out = $('#fw-upload-out');
  const f = $('#fw-file').files && $('#fw-file').files[0];

  out.textContent = '';
  if (!f) { out.append(el('div', 'bad', 'Pick the tarball first.')); return; }

  const btn = $('#fw-upload');
  const bar = el('div', 'progress');
  const fill = el('div', 'fill');

  bar.append(fill);
  out.append(el('div', null, `Uploading ${f.name} (${bytes(f.size)})…`));
  out.append(bar);
  btn.disabled = true;

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/upload');
  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) fill.style.width = Math.round((e.loaded / e.total) * 100) + '%';
  };
  xhr.onload = () => {
    btn.disabled = false;
    let j = {};
    try { j = JSON.parse(xhr.responseText); } catch (e) { /* reported below */ }
    if (!j.ok) {
      out.append(el('div', 'bad', j.error || `upload failed: HTTP ${xhr.status}`));
      return;
    }
    UPLOADED = { name: f.name, bytes: j.bytes, md5: j.md5 };
    out.append(el('div', 'good', `${bytes(j.bytes)} received.`));
    /* The device's own md5, to compare with the one beside the image you
       built. The updater checks the kernel and rootfs md5s from inside the tar
       before it erases anything, so this is about the transfer. */
    if (j.md5) out.append(el('div', 'mono', 'md5 ' + j.md5));
    renderFirmware();
  };
  xhr.onerror = () => {
    btn.disabled = false;
    out.append(el('div', 'bad', 'The upload did not complete.'));
  };
  xhr.send(f);
}

/*
 * What a slot holds, judged by the kernel header the flash wrote into it
 * (/api/firmware `slots`), never by U-Boot sw_version<p>, which is only what
 * the last updater chose to record and has named an odi-oss build for a slot
 * holding the stock firmware. The stock V1.0-220923 base names its kernel
 * "Linux Kernel Image"; odi-oss adds the kernel line, "... 6.18".
 */
function slotKind(fw, p) {
  const k = ((fw.slots || {})[p] || {}).kernel;
  if (!k) return { kind: 'unknown' };
  const built = ((fw.slots || {})[p] || {}).built;
  const when = built ? new Date(built * 1000).toISOString().slice(0, 10) : '';
  if (/\b([6-9]|\d{2})\.\d+\b/.test(k)) return { kind: 'odi', kernel: k, when };
  return { kind: k.trim() === 'Linux Kernel Image' ? 'stock' : 'other', kernel: k, when };
}

function describeSlot(fw, p) {
  const k = slotKind(fw, p);
  const rec = (fw.env || {})['sw_version' + p];
  const bits = [];
  if (k.kernel) bits.push(`kernel \u201c${k.kernel}\u201d${k.when ? ', built ' + k.when : ''}`);
  if (rec) bits.push('U-Boot records ' + rec);
  return bits.join('; ');
}

async function writeImage(part, fw) {
  const out = $('#fwout');
  const k = slotKind(fw || {}, part);

  if (!confirm(`Write the uploaded image to partition ${part}?\n\n`
      + 'This takes about eighty seconds and the stick must not lose power '
      + 'meanwhile. The partition you are running now is not touched, and '
      + 'nothing boots the new one until you try it.')) return;
  /* A second question, only when there is something to lose: the other slot
     is where the stock firmware is kept as the fallback on most sticks, and
     writing it is a choice that cannot be taken back from this page. */
  if (k.kind !== 'odi' && !confirm(`Partition ${part} holds `
      + (k.kind === 'stock' ? 'the stock firmware' : k.kind === 'other' ? 'an image that is not odi-oss'
        : 'an image this page cannot identify')
      + ` (${describeSlot(fw || {}, part) || 'no details'}).\n\n`
      + 'Writing replaces it, and you lose it as a fallback. Overwrite it?')) return;

  out.textContent = '';
  out.append(el('div', 'warn', `Writing partition ${part}.`));
  try {
    const r = await fetch('/api/firmware', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ action: 'write', partition: part }).toString(),
    });
    const j = await r.json();
    if (j.output) out.append(el('pre', null, String(j.output).trim()));
    if (!j.ok) {
      out.append(el('div', 'bad', j.error || 'the updater refused'));
      return;
    }
    /* An image whose starter writes in the background leaves a state in
       /api/firmware to follow; one whose starter blocks has already
       finished, and its answer above is the whole story. */
    await followWrite(part);
  } catch (e) {
    out.append(el('div', 'bad', String(e.message || e)));
  }
}

/* Poll a background write until it says ok or failed. One poll at a time:
   a re-render while writing would otherwise start another. */
async function followWrite(part) {
  if (FOLLOWING) return;
  FOLLOWING = true;
  try { await followLoop(part); } finally { FOLLOWING = false; }
  renderFirmware();
}

async function followLoop(part) {
  const out = $('#fwout');
  const log = el('pre', null, '');
  out.append(log);
  for (;;) {
    let fw;
    try { fw = await get('/api/firmware'); } catch (e) { fw = {}; }
    const w = fw.write;
    if (!w) {
      out.append(el('div', 'good', `Partition ${part} written. Press Try on it — a `
        + 'trial boots once, and reverts by itself if the image does not come up.'));
      break;
    }
    log.textContent = String(w.log || '').trim();
    if (w.state === 'ok') {
      out.append(el('div', 'good', `Partition ${w.slot} written and read back. Press Try `
        + 'on it — a trial boots once, and reverts by itself if the image does not '
        + 'come up.'));
      break;
    }
    if (w.state === 'failed') {
      out.append(el('div', 'bad', `The write of partition ${w.slot} failed`
        + (w.rc ? ` (exit ${w.rc})` : '') + '. The log above says whether it got as '
        + 'far as erasing; the running partition was not touched.'));
      break;
    }
    await new Promise((res) => setTimeout(res, WRITE_POLL_MS));
  }
}

async function renderFirmware() {
  const host = $('#parts');
  let fw;
  try {
    fw = await get('/api/firmware');
  } catch (e) { fail(e); return; }
  S.FW = fw;
  renderTrial();
  host.textContent = '';

  const env = fw.env || {};
  const committed = env.sw_commit;
  const booted = env.sw_active;
  const pending = env.sw_tryactive;
  const writing = fw.write && fw.write.state === 'running';

  const grid = el('div', 'slots');
  for (const p of ['0', '1']) {
    const k = slotKind(fw, p);
    const card = el('section', 'slot' + (p === booted ? ' running' : ''));
    const head = el('div', 'slot-head');
    head.append(el('h2', null, 'Partition ' + p));
    const tags = el('span', 'slot-tags');
    if (p === booted) tags.append(el('span', 'tag live', 'Running'));
    if (p === committed) tags.append(el('span', 'tag', 'Committed'));
    if (p === pending && pending !== '2') tags.append(el('span', 'tag warn', 'Trial pending'));
    head.append(tags);
    card.append(head);

    /*
       The running partition shows what it runs: the image name from the build
       manifest, or /etc/version. sw_version<p> is only what the updater last
       recorded, and odi-oss fwu.sh records nothing unless asked.
    */
    const dl = el('dl', 'slot-kv');
    const kindText = { odi: 'odi-oss', stock: 'Stock firmware', other: 'Not odi-oss', unknown: 'Unknown' }[k.kind];
    if (p === booted) {
      const running = (fw.build || {}).image || fw.running || env['sw_version' + p] || 'unknown';
      dl.append(el('dt', null, 'Image'), el('dd', 'strong', running));
    } else {
      dl.append(el('dt', null, 'Holds'), el('dd', 'strong', kindText));
    }
    if (k.kernel) dl.append(el('dt', null, 'Kernel'), el('dd', null, k.kernel + (k.when ? ', built ' + k.when : '')));
    const rec = env['sw_version' + p];
    if (rec) dl.append(el('dt', null, 'U-Boot records'), el('dd', 'mono', rec));
    card.append(dl);

    const acts = el('div', 'slot-acts');
    if (p !== booted && (k.kind === 'stock' || k.kind === 'other')) {
      card.append(el('p', 'callout warn', `Partition ${p} holds `
        + (k.kind === 'stock' ? 'the stock firmware' : 'an image that is not odi-oss')
        + '. Writing replaces it, and you lose it as a fallback.'));
    }
    /* Try only the slot you are NOT running: a trial of the running one
       boots what is already up. Committing the running slot is the same act
       as the trial banner Commit to this image, and goes through the same code. */
    if (p !== committed && p !== booted) {
      const b = el('button', null, 'Try partition ' + p);
      b.type = 'button';
      b.onclick = () => fwAction('try', p,
        `Partition ${p} will boot once, at the next reboot. If it fails, the stick `
        + `returns to partition ${committed}, the committed one, on its own.`);
      acts.append(b);
    }
    if (p !== committed && p === booted) {
      const b = el('button', 'primary', 'Commit to partition ' + p);
      b.type = 'button';
      b.onclick = () => commitRunning();
      acts.append(b);
    }
    /*
       Offered only for the partition this stick is NOT running. The daemon
       refuses the running one as well -- fwu.sh would too -- but finding that
       out after the erase has started is not where anyone should learn it.
       `booted !== undefined` matters: with sw_active unreadable this once
       offered to write BOTH partitions, including the running one.
    */
    if (booted !== undefined && p !== booted && !writing) {
      if (UPLOADED) {
        const b = el('button', 'danger', 'Write the uploaded image here');
        b.type = 'button';
        b.onclick = () => writeImage(p, fw);
        acts.append(b);
      } else {
        acts.append(el('span', 'hint', 'Upload an image below to write it here.'));
      }
    }
    if (acts.children.length) card.append(acts);
    grid.append(card);
  }
  host.append(grid);
  /* The explanation sits right under the partitions it is about. It is
     static markup in index.html, held here because the host is rebuilt on
     every render and would otherwise drop it. */
  COMMIT_WHY = COMMIT_WHY || $('#commit-why');
  if (COMMIT_WHY) host.append(COMMIT_WHY);

  /* Spell the commands out with this stick's own address and the partition it
     is not running, so they can be pasted without being adapted. */
  const other = booted === '0' ? '1' : '0';
  $('#upload').textContent = [
    '# on your machine, in the odi-oss checkout',
    'make image                     # -> out/image/<version>.tar',
    '',
    'IMG=out/image/<version>.tar',
    `cat "$IMG" | ssh root@${location.hostname} 'cat > /tmp/img.tar'`,
    `md5 -q "$IMG"; ssh root@${location.hostname} 'md5sum /tmp/img.tar'`,
    '',
    `# writes partition ${other}, the one this stick is not running, and waits`,
    `ssh root@${location.hostname} '/etc/scripts/fwu_starter.sh --foreground ${other} /tmp/img.tar'`,
  ].join('\n');

  /* What this image was built from, and which confd is answering: a binary
     at /etc/config/confd/confd overrides the image copy and survives
     reflashing, so the two drift apart silently. */
  const bl = el('section', 'block build');
  const bh = el('div', 'block-head');
  bh.append(el('h2', null, 'This image'));
  const trial = booted !== undefined && committed !== undefined && booted !== committed;
  /* The class badge is the button: its label IS the cost. */
  const rb = el('button', 'danger cost-reboot');
  rb.type = 'button';
  rb.append(el('i', 'bars'), document.createTextNode('Reboot'));
  rb.onclick = () => fwAction('reboot', '',
    'The stick reboots and the fibre service drops for about two minutes.'
    + (trial ? ` This is a trial of partition ${booted}: the reboot comes back on `
      + `partition ${committed}, the committed one, not on this image.` : ''));
  bh.append(rb);
  bl.append(bh);
  const b = fw.build || {};
  const kv = el('dl', 'kv');
  for (const key of ['image', 'base', 'confd', 'exporter', 'built']) {
    if (key in b) kv.append(el('dt', null, key === 'confd' ? 'Config UI' : key[0].toUpperCase() + key.slice(1)),
      el('dd', 'mono', b[key]));
  }
  kv.append(el('dt', null, 'Answering'),
    el('dd', 'mono', 'confd ' + (fw.confd || 'unknown') + (fw.exe ? ', ' + fw.exe : '')));
  bl.append(kv);
  /*
     An override is decided by WHERE the running confd came from, not by
     comparing build ids: a local build stamps the manifest `confd=local`,
     which matches no id, and the page used to call the image own /bin/confd
     an override on every such build. scripts/deploy.sh installs an override
     in /etc/config/confd/ (which is /var/config), and only an image that
     ships its own confd -- one with a manifest -- can be overridden.
  */
  const override = /^\/(etc|var)\/config\//.test(fw.exe || '');
  if (!Object.keys(b).length) {
    bl.append(el('p', 'hint', 'No /etc/odi-build in this image, so it predates build manifests.'));
  } else if (override) {
    bl.append(el('p', 'callout warn',
      `The config UI answering runs from ${fw.exe}, not the copy this image ships: `
      + 'an override in /etc/config is in use, and it survives reflashing.'));
  }
  host.append(bl);

  if (fw.mem) $('#memtotal').textContent = fw.mem;

  if (writing) {
    host.append(el('p', 'callout warn', `Partition ${fw.write.slot} is being written. `
      + 'Do not reboot or power off until it finishes.'));
    followWrite(fw.write.slot);
  }
}

/* Set while a write is being followed, so a re-render does not start a
   second poll loop. */
let FOLLOWING = false;

async function fwAction(action, partition, warning) {
  const out = $('#fwout');
  out.textContent = '';
  if (!confirm((action === 'reboot' ? '\u25AE'.repeat(CLASS_RANK.reboot) + ' ' + CLASS_LABEL.reboot + '\n\n' : '') + warning + '\n\nContinue?')) return;
  try {
    const r = await fetch('/api/firmware', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'action=' + action + '&partition=' + encodeURIComponent(partition),
    });
    const res = await r.json();
    if (!res.ok) { out.append(el('div', 'bad', res.error || 'failed')); return; }

    if (action === 'try') {
      out.append(el('div', 'warn',
        `Partition ${partition} is armed for one boot. Reboot to try it; if it ` +
        `does not come up the stick returns here on its own.`));
    } else if (action === 'commit') {
      out.append(el('div', 'good', `Committed: partition ${partition} is now the one it boots.`));
    } else {
      out.append(el('div', 'warn', 'Rebooting. This page will stop responding.'));
    }
    if (action !== 'reboot') await renderFirmware();
  } catch (e) {
    out.append(el('div', 'bad', String(e.message || e)));
  }
}

function wireFirmware() {
  $('#fw-upload').onclick = uploadImage;
}

/*
 * The trial-boot banner.
 *
 * A trial is any state in which U-Boot would boot a partition other than the
 * running one at the next reboot: sw_commit, in either copy of the redundant
 * environment, naming another slot. Both copies count, because a plain
 * `nv setenv` writes only the winning one and U-Boot falls back to the other
 * if the winner is ever left invalid. It stays up until the condition is
 * gone; there is nothing to dismiss.
 */
function renderTrial() {
  const fw = S.FW || {};
  const env = fw.env || {};
  const fb = fw.fallback || {};
  const run = env.sw_active;
  const bar = $('#trialbanner');
  const main = env.sw_commit;
  const alt = fb.sw_commit;
  const trial = run !== undefined && [main, alt].some((c) => c !== undefined && c !== run);
  bar.hidden = !trial;
  if (!trial) return;

  const back = main !== undefined && main !== run ? main : alt;
  const k = slotKind(fw, back);
  const holds = { odi: 'an odi-oss image', stock: 'the stock firmware', other: 'an image that is not odi-oss',
                  unknown: 'an image this page cannot identify' }[k.kind];
  const rec = env['sw_version' + back];
  /* Generic on purpose: no claim about what the other slot holds. It is not
     always the stock firmware, and System > Firmware says what it is. */
  $('#trial-what').textContent = main !== run
    ? `This image (partition ${run}) is not committed. The next reboot returns to partition ${back}.`
    : `Partition ${run} is committed in the active U-Boot copy, but the fallback copy still names `
      + `partition ${back}. If the active copy is ever lost, the stick boots that.`;

  const commit = $('#trial-commit');
  commit.hidden = main === run;
  commit.onclick = () => commitRunning();
}

/*
 * Commit to the running image: sw_commit to the running slot. One path, used by
 * the trial banner and by the running slot on the Firmware subtab, so the
 * two cannot drift into saying or doing different things.
 */
async function commitRunning() {
  const env = (S.FW || {}).env || {};
  const run = env.sw_active;
  const back = env.sw_commit;
  const out = $('#trial-out');
  out.textContent = '';
  if (run === undefined) return;
  if (!confirm(`Commit to partition ${run}?\n\nIt becomes the one the stick boots by default`
      + (back !== undefined && back !== run ? `, instead of partition ${back}.` : '.')
    + ' If it later fails to come up, the stick keeps booting it: there is no trial to fall back from.')) return;
  const btn = $('#trial-commit');
  btn.disabled = true;
  try {
    const r = await fetch('/api/firmware', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'action=commit&partition=' + encodeURIComponent(run),
    });
    const res = await r.json();
    if (!res.ok) { out.append(el('div', 'bad', res.error || 'not committed')); return; }
    S.FW = await get('/api/firmware');
    renderTrial();
    if (!$('#trialbanner').hidden) {
      out.append(el('div', 'bad', 'Committed, but the stick still reports a trial: '
        + `over SSH, nv getenv; nv fallback show which copy still says something other than sw_commit=${run}.`));
    } else if (res.refreshed === false) {
      out.append(el('div', null, 'Committed in both environment copies. The exporter and the login notice '
        + 'still show the old state until slot-state.sh runs (over SSH) or the next reboot.'));
    }
    if (!$('#p-system').hidden) await renderFirmware();
  } catch (e) {
    out.append(el('div', 'bad', String(e.message || e)));
  } finally { btn.disabled = false; }
}

export { renderFirmware, wireFirmware, slotKind, renderTrial };
