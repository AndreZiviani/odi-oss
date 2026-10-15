/*
 * The two diagnostics that need a device rather than a config file: what the
 * kernel has been complaining about, and whether this stick can reach anything.
 */

import { $, el, get } from './dom.js';

/*
 * Kernel messages carry a priority in angle brackets. The kernel writes a
 * bare level, <4>; a userland line written to /dev/kmsg carries its syslog
 * facility too, <12> being user.warning, so the level is the low three bits
 * and the prefix is never shown. The RT_ERR_ switch errors arrive as bare
 * continuation lines under a <4> header, which is why a line with no prefix
 * inherits the last one seen.
 */
const LEVELS = ['emerg', 'alert', 'crit', 'err', 'warn', 'notice', 'info', 'debug'];

/* The two once-a-minute liveness lines (the boot script and the watchdog)
   are most of the buffer on a healthy stick, and hide the lines that say
   something. Hidden by default, counted, one tick away. */
const HEARTBEAT = /^(rcS|odi_wdt): alive\b/;

function renderLogLines(text, host, opts = {}) {
  let level = null;
  let hidden = 0;

  for (const raw of String(text || '').split('\n')) {
    const line = raw.replace(/\r$/, '');
    if (!line.trim()) continue;

    const m = /^<(\d{1,3})>(.*)$/.exec(line);
    let body = line;
    if (m) { level = Number(m[1]) & 7; body = m[2]; }
    /* A bare `<4>` with nothing after it is real -- the kernel emits them --
       and rendering it as an empty row is noise. Take the level from it and
       drop the row. */
    if (!body.trim()) continue;
    if (!opts.heartbeats && HEARTBEAT.test(body)) { hidden++; continue; }

    const cls = level === null ? '' : level <= 3 ? 'bad' : level === 4 ? 'warn' : '';
    const row = el('div', 'logline ' + cls);
    row.append(el('span', 'lvl', level === null ? '' : LEVELS[level]), el('span', 'msg', body));
    host.append(row);
  }
  return { hidden };
}

async function renderLog() {
  const host = $('#log-out');

  host.textContent = 'Reading…';
  let d;
  try { d = await get('/api/log'); } catch (e) {
    host.textContent = '';
    host.append(el('p', 'callout bad', 'Could not read the log: ' + String(e.message || e)));
    return;
  }
  host.textContent = '';

  if (d.error) { host.append(el('p', 'callout bad', d.error)); return; }
  if (!String(d.raw || '').trim()) {
    host.append(el('p', 'hint', 'The ring buffer is empty.'));
    return;
  }
  if (d.truncated)
    host.append(el('p', 'hint', 'Only the newest part of the buffer fits here; the oldest lines were dropped.'));

  const box = el('div', 'log');
  box.tabIndex = 0;
  box.setAttribute('aria-label', 'Kernel log');
  const r = renderLogLines(d.raw, box, { heartbeats: $('#log-heartbeat').checked });
  host.append(box);
  if (r.hidden) host.append(el('p', 'hint', r.hidden + ' heartbeat line' + (r.hidden === 1 ? '' : 's') + ' hidden.'));
  /* Newest last, like a terminal. Scroll there so a refresh lands on what just
     happened rather than on what happened at boot. */
  box.scrollTop = box.scrollHeight;
}

async function runPing() {
  const out = $('#ping-out');
  const btn = $('#ping-go');
  const host = $('#ping-host').value.trim();

  out.textContent = '';
  if (!host) return;
  btn.disabled = true;
  out.append(el('div', 'hint', `Pinging ${host}\u2026`));
  try {
    const r = await fetch('/api/ping', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ host }).toString(),
    });
    const j = await r.json();
    if (j.output) out.append(el('pre', null, j.output.trim()));
    /* An error from the daemon and a silent host are different findings: one
       sends you to the image, the other to the network. */
    if (j.error) out.append(el('div', 'bad', j.error));
    else if (!j.ok) out.append(el('div', 'bad', 'No reply.'));
  } catch (e) {
    out.append(el('div', 'bad', String(e.message || e)));
  } finally {
    btn.disabled = false;
  }
}

let wired = false;

function renderTools() {
  if (!wired) {
    wired = true;
    $('#log-reload').onclick = renderLog;
    $('#log-heartbeat').onchange = renderLog;
    $('#ping-go').onclick = runPing;
    $('#ping-host').onkeydown = (e) => { if (e.key === 'Enter') runPing(); };
  }
  renderLog();
}

export { renderLogLines, renderTools };
