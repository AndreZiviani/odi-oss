/*
 * The Forwarding panel: the only place on this device where you can see whether
 * traffic is actually crossing the ONU.
 */

import { $, el, bytes } from './dom.js';

/*
 * Port 2 faces the fibre and port 0 the host, established by correlating their
 * deltas on two different lines. Facing them at each other makes the mirror —
 * what one receives, the other sends — the thing you read first, because that
 * is the actual test for whether the stick is forwarding.
 *
 * THROUGHPUT, not totals. omci_app clears port 0's counters at every OMCI
 * performance-monitoring interval while port 2's run free, so their cumulative
 * figures diverge by orders of magnitude — 14 GB against 58 MB on a stick that
 * is forwarding perfectly. Side by side that reads as a fault. The rates
 * between polls do mirror, which is the thing actually worth showing.
 *
 * A negative delta means the counter was reset between polls, so that sample is
 * dropped rather than rendered as a spike.
 */
let PREV = null;
function renderFlow(text) {
  const ports = {};
  let cur = null;
  for (const line of text.split('\n')) {
    const p = /^Port:\s*(\d+)/.exec(line);
    if (p) { cur = p[1]; ports[cur] = {}; continue; }
    const kv = /^(\w+)\s*:\s*(\d+)\s*$/.exec(line);
    if (kv && cur !== null) ports[cur][kv[1]] = kv[2];
  }
  const now = Date.now();
  const rate = (id, k) => {
    /* Both sides need the `|| {}`. side() had it and this did not, so a poll
       that reported one port and not the other — a truncated counter dump, or
       a diag hiccup — threw a TypeError out of renderStatus, and the whole
       status page froze on the previous sample while the timer kept retrying
       the same throw every 15 seconds. */
    if (!PREV || !PREV.ports[id] || !ports[id]) return null;
    const dt = (now - PREV.at) / 1000;
    if (dt < 1) return null;
    const d = Number(ports[id][k] || 0) - Number(PREV.ports[id][k] || 0);
    return d < 0 ? null : d / dt;    /* negative means the counter was reset */
  };

  const side = (id, role, cls) => {
    const d = el('div', 'side' + (cls ? ' ' + cls : ''));
    d.append(el('div', 'role', role), el('div', 'port', 'Port ' + id));
    const dl = el('dl');
    const p = ports[id] || {};
    for (const [k, label] of [['ifInOctets', 'in'], ['ifOutOctets', 'out']]) {
      const r = rate(id, k);
      dl.append(el('dt', null, label),
                el('dd', 'num', r === null ? 'measuring' : bytes(r) + '/s'));
    }
    dl.append(el('dt', null, 'dropped'),
              el('dd', 'num', Number(p.dot1dTpPortInDiscards || 0).toLocaleString()));
    d.append(dl);
    return d;
  };

  const host = $('#flow');
  host.textContent = '';
  if (!Object.keys(ports).length) {
    host.append(el('p', 'hint', 'No counters reported.'));
    PREV = null;
    return;
  }
  host.append(side('2', 'Fibre'), el('div', 'mirror', '\u21c4'), side('0', 'Host', 'right'));
  PREV = { at: now, ports };
}

export { renderFlow };
