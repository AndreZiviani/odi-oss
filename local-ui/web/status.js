/*
 * The Status tab: optical meters and the registration ladder.
 *
 * The device parses nothing it does not have to -- /api/status returns diag's
 * raw output and the splitting happens here, which means adding a status field
 * costs no C and no reflash.
 */

import { $, el } from './dom.js';
import { renderFlow } from './flow.js';

/* diag echoes each command after its "RTK.0> " prompt, so the reply splits back
 * into per-command sections on that marker. */
function sections(raw) {
  const out = {};
  const parts = raw.split('RTK.0> ');
  for (const p of parts) {
    const nl = p.indexOf('\n');
    if (nl < 0) continue;
    out[p.slice(0, nl).trim()] = p.slice(nl + 1);
  }
  return out;
}

const num = (s, re) => { const m = re.exec(s || ''); return m ? m[1] : null; };

/*
 * Optical power, on a scale.
 *
 * A bare number does not tell you whether -23 dBm is healthy. The window drawn
 * on the scale is the GPON class B+ range this optics is specified for, and
 * the two dimension lines under it are the margin to each edge -- which is
 * the number that actually decides whether a line will survive a dirty
 * connector or a new splice. The edge names are the ones a power-meter
 * datasheet uses: sensitivity is the weakest light the receiver still
 * decodes, overload the strongest.
 */
/*
 * The windows are the ONU figures for optical budget class B+ in ITU-T
 * G.984.2 Amendment 1: mean launched power +0.5 to +5 dBm, receiver
 * sensitivity -27 dBm, overload -8 dBm. (The OLT side of B+ differs --
 * +1.5 to +5 launched, -28 sensitivity -- and is not what this stick is.)
 */
const OPTICS = {
  rx: { name: 'Receive', lo: -30, hi: -4, winLo: -27, winHi: -8,
        loName: 'sensitivity', hiName: 'overload', note: 'Class B+ receive window' },
  tx: { name: 'Transmit', lo: -3, hi: 8, winLo: 0.5, winHi: 5,
        loName: 'minimum', hiName: 'maximum', note: 'Class B+ launch power' },
};
/*
 * "Near the edge" is the outer 16% of the window at each end, not a fixed
 * figure: about 3 dB of the 19 dB receive window, about 0.7 dB of the
 * 4.5 dB launch window. A fixed 3 dB was more than half the launch window,
 * so a healthy +2.15 dBm transmitter read as marginal on every stick.
 */
const TIGHT_SHARE = 0.16;
const tight = (spec) => (spec.winHi - spec.winLo) * TIGHT_SHARE;

/* A real minus sign, not a hyphen: the readout face draws the two differently. */
const signed = (v, d) => (v < 0 ? '−' : '') + Math.abs(v).toFixed(d);

function meter(hostSel, spec, value) {
  const host = $(hostSel);
  host.textContent = '';
  const pct = (v) => ((Math.max(spec.lo, Math.min(spec.hi, v)) - spec.lo) / (spec.hi - spec.lo)) * 100;

  let verdict = 'none';
  let say = 'No reading';
  if (value !== null) {
    const m = Math.min(value - spec.winLo, spec.winHi - value);
    if (value < spec.winLo || value > spec.winHi) { verdict = 'bad'; say = 'Out of window'; }
    else if (m < tight(spec)) { verdict = 'warn'; say = 'Near the edge'; }
    else { verdict = 'ok'; say = 'In window'; }
  }
  host.className = 'meter v-' + verdict;

  const head = el('div', 'm-head');
  head.append(el('h2', 'm-name', spec.name), el('span', 'm-verdict', say));
  host.append(head);

  const read = el('div', 'm-read');
  read.append(el('span', 'num', value === null ? '—' : signed(value, 2)), el('span', 'unit', 'dBm'));
  host.append(read);

  const scale = el('div', 'scale');
  scale.style.setProperty('--n', String(spec.hi - spec.lo));
  const win = el('div', 'win');
  win.style.left = pct(spec.winLo) + '%';
  win.style.width = (pct(spec.winHi) - pct(spec.winLo)) + '%';
  scale.append(el('div', 'ticks'), win);
  if (value !== null) {
    const n = el('div', 'needle');
    n.style.left = pct(value) + '%';
    scale.append(n);
  }
  const lab = el('div', 'm-ticks');
  const step = spec.hi - spec.lo > 15 ? 5 : 2;
  for (let v = Math.ceil(spec.lo / step) * step; v <= spec.hi; v += step) {
    const t = el('span', null, signed(v, 0));
    t.style.left = pct(v) + '%';
    lab.append(t);
  }

  /* Dimension lines: edge to needle, both sides. Outside the window there is
     only one, from the edge that was crossed, and it says by how much. */
  const dims = el('div', 'm-dims');
  const dim = (a, b, text, cls) => {
    const d = el('div', 'dim' + (cls ? ' ' + cls : ''));
    d.style.left = Math.min(pct(a), pct(b)) + '%';
    d.style.width = Math.abs(pct(b) - pct(a)) + '%';
    /* The figure always shows; the edge name goes first when the line is
       too short to hold both (a narrow screen, a reading near an edge). */
    const [fig, ...rest] = text.split(' dB ');
    const t = el('span', null, fig + ' dB');
    t.append(el('em', null, ' ' + rest.join(' dB ')));
    d.append(t);
    dims.append(d);
  };
  let label = spec.name + ': no reading.';
  if (value !== null) {
    const toLo = value - spec.winLo;
    const toHi = spec.winHi - value;
    if (toLo < 0) {
      dim(value, spec.winLo, `${(-toLo).toFixed(1)} dB below ${spec.loName}`, 'over');
      label = `${signed(value, 2)} dBm, ${(-toLo).toFixed(1)} dB below the ${spec.loName} edge.`;
    } else if (toHi < 0) {
      dim(spec.winHi, value, `${(-toHi).toFixed(1)} dB over ${spec.hiName}`, 'over');
      label = `${signed(value, 2)} dBm, ${(-toHi).toFixed(1)} dB over the ${spec.hiName} edge.`;
    } else {
      dim(spec.winLo, value, `${toLo.toFixed(1)} dB to ${spec.loName}`, toLo < tight(spec) ? 'tight' : '');
      dim(value, spec.winHi, `${toHi.toFixed(1)} dB to ${spec.hiName}`, toHi < tight(spec) ? 'tight' : '');
      label = `${signed(value, 2)} dBm: ${toLo.toFixed(1)} dB above ${spec.loName}, `
        + `${toHi.toFixed(1)} dB below ${spec.hiName}.`;
    }
  }
  const inst = el('div', 'instrument');
  inst.setAttribute('role', 'img');
  inst.setAttribute('aria-label', label + ' ' + spec.note + ', '
    + signed(spec.winLo, 1) + ' to ' + signed(spec.winHi, 1) + ' dBm.');
  inst.append(scale, lab, dims);
  host.append(inst);
  const foot = el('p', 'm-note');
  foot.textContent = `${spec.note}, ${signed(spec.winLo, 1)} to ${signed(spec.winHi, 1)} dBm`;
  host.append(foot);
  return { verdict, value };
}

/* O1 to O5 is a real sequence — an ONU climbs it on every registration — which
   is the one thing on this page worth numbering. */
const STATES = {
  O1: 'Initial', O2: 'Standby', O3: 'Serial number',
  O4: 'Ranging', O5: 'Operational', O6: 'Intermittent', O7: 'Emergency stop',
};

function ladder(state) {
  const host = $('#ladder');
  host.textContent = '';
  const n = state ? Number(state.slice(1)) : 0;
  for (let i = 1; i <= 5; i++) {
    const li = el('li', null, 'O' + i);
    li.title = STATES['O' + i];
    if (i === n) li.setAttribute('aria-current', 'step');
    if (i < n) li.classList.add('done');
    if (i === n) li.classList.add('here');
    host.append(li);
  }
  const note = $('#statenote');
  note.classList.toggle('bad', state !== 'O5');
  note.textContent = !state ? 'No state reported.'
    : state === 'O5' ? 'Operational. The line is registered and carrying traffic.'
    : (STATES[state] || state) + ': not yet operational.';
}
function renderStatus(raw) {
  const s = sections(raw);
  $('#raw').textContent = raw;

  const dbm = (key) => {
    const v = num(s['pon get transceiver ' + key], /:\s*(-?\d+\.\d+)/);
    return v === null ? null : Number(v);
  };
  const rx = meter('#rx', OPTICS.rx, dbm('rx-power'));
  meter('#tx', OPTICS.tx, dbm('tx-power'));

  const state = num(s['gpon get onu-state'], /Operation State\((O\d)\)/);
  ladder(state);

  const alarms = (s['gpon get alarm-status'] || '').split('\n').filter((l) => /Alarm /.test(l));
  const asserted = alarms.filter((l) => !/clear/i.test(l));

  /*
   * Loss of signal costs you more than the fibre. The stick reports RX_LOS on
   * its SFP pins, and most hosts -- Mikrotik and Ubiquiti among them -- take
   * that as "this transceiver has nothing to say" and disable the Ethernet
   * side, so the management page goes with it. That is the single most common
   * "my stick is bricked" report on this hardware, and it is not a fault in
   * the stick: it is the host doing what a plain transceiver would want.
   *
   * Worth saying HERE, while the page is still reachable, because the moment
   * it applies is the moment you cannot read it.
   */
  const los = $('#losnote');
  const lost = asserted.some((l) => /\bLO[SF]\b/i.test(l));
  los.hidden = !lost;
  los.classList.toggle('bad', lost);
  if (lost) {
    los.textContent = 'Loss of signal. Note that most SFP hosts disable the '
      + 'Ethernet side of the cage while a transceiver asserts RX_LOS, so this '
      + 'page may become unreachable until the fibre is back — through the '
      + 'host, not through the stick. A media converter reaches it either way.';
  }
  const stat = (k, v, cls) => [el('dt', null, k), el('dd', cls || null, v)];
  const temp = num(s['pon get transceiver temperature'], /:\s*(-?\d+\.\d+)/);
  const volt = num(s['pon get transceiver voltage'], /:\s*(-?\d+\.\d+)/);
  const env = $('#env');
  env.textContent = '';
  env.append(
    ...stat('Temperature', temp === null ? '\u2014' : signed(Number(temp), 1) + ' \u00b0C', 'num'),
    ...stat('Supply', volt === null ? '\u2014' : Number(volt).toFixed(2) + ' V', 'num'),
    ...stat('Alarms',
      !alarms.length ? '\u2014' : asserted.length ? asserted.length + ' asserted' : 'All clear',
      asserted.length ? 'bad' : (alarms.length ? 'ok' : '')),
  );
  if (asserted.length) {
    env.append(el('dt', null, 'Asserted'),
      el('dd', 'bad', asserted.map((l) => l.replace(/\s*Alarm\s*:.*$/i, '').trim()).join(', ')));
  }

  /* The header pill: the two facts that say the line is up, on every view. */
  const pill = $('#linepill');
  pill.hidden = false;
  pill.textContent = '';
  const ok = state === 'O5' && rx.verdict !== 'bad' && rx.verdict !== 'none';
  pill.className = 'linepill ' + (ok ? (rx.verdict === 'warn' ? 'warn' : 'ok') : 'bad');
  pill.append(el('i', 'dot'), el('span', null, state || 'No state'));
  if (rx.value !== null) pill.append(el('span', 'num', signed(rx.value, 1) + ' dBm'));
  pill.title = ok ? 'Registered, receive level in window' : 'Not healthy: see Status';

  renderFlow(s['mib dump counter port all'] || '');
}

export { renderStatus, OPTICS, tight };
