/*
 * What a value has to satisfy before it is worth sending.
 *
 * These mirror the daemon's checks rather than replacing them: anything
 * reachable over HTTP is validated there too, since a request need not come
 * from this page. What they buy is telling the user before the write instead of
 * after it.
 */

import { S, EDITS, dependsUnmet } from './state.js';

function rangeBad(row, raw) {
  const m = S.META[row.name];
  if (!m || !m.range || raw === '' || raw === undefined) return null;
  const [lo, hi] = m.range.split('-').map(Number);
  const n = Number(raw);
  if (!Number.isFinite(n) || n < lo || n > hi) return `outside ${lo}\u2013${hi}`;
  return null;
}

/*
 * Everything wrong with a value, in one place, so the field marking and the
 * Save gate cannot disagree about what is acceptable.
 *
 * These checks mirror the daemon's rather than replacing them — anything
 * reachable over HTTP is validated there too, since a request need not come
 * from this page. What they buy is telling the user before the write instead of
 * after it.
 */
function valueProblem(row, raw) {
  /* hostport keys (SYSLOG_SERVER, NTP_SERVER), ascii14 (ONU_HW_VERSION) and
     fixedme (OMCI_FIXED_ME) are odi-only and empty clears them. */
  if (['hostport', 'ascii14', 'fixedme'].includes(row.type) && raw === '') return null;
  if (raw === '' || raw === undefined) {
    return 'cannot be empty \u2014 flash set refuses to clear a key';
  }
  if (row.type === 'int' && !/^\d+$/.test(raw)) return 'must be a whole number';
  if (row.type === 'ipv4') {
    const oct = raw.split('.');
    if (oct.length !== 4 || !oct.every((o) => /^\d{1,3}$/.test(o) && Number(o) <= 255)) {
      return 'must be four numbers 0\u2013255 separated by dots';
    }
  }
  if (row.type === 'hostport') {
    const m = /^([A-Za-z0-9.-]+)(?::(\d{1,5}))?$/.exec(raw);
    if (!m || /^[.-]|[.-]$/.test(m[1]) || m[1].length > 253
        || (m[2] !== undefined && (Number(m[2]) < 1 || Number(m[2]) > 65535))) {
      return 'must be a host or host:port: letters, digits, dots and hyphens, '
        + 'port 1\u201365535, no spaces or quotes';
    }
  }
  if (row.type === 'ascii14' && !/^[\x20-\x7e]{1,14}$/.test(raw)) {
    return 'at most 14 printable ASCII characters';
  }
  if (row.type === 'ascii14' && /["<&]/.test(raw)) {
    return 'a quote, < or & cannot be stored';
  }
  if (row.type === 'fixedme') {
    const bad = 'up to 8 CLASS:HEX entries separated by commas or spaces: class 1\u201365535, '
      + 'hex of 1 to 25 bytes (an even count of digits), no class twice';
    if (!/^[0-9]{1,5}:(0[xX])?([0-9a-fA-F]{2}){1,25}([ ,]+[0-9]{1,5}:(0[xX])?([0-9a-fA-F]{2}){1,25})*$/.test(raw)) return bad;
    const classes = raw.split(/[ ,]+/).map((e) => Number(e.split(':')[0]));
    if (classes.length > 8 || classes.some((c) => c < 1 || c > 65535)
        || new Set(classes).size !== classes.length) return bad;
  }
  if (row.type === 'mac' && !/^[0-9a-fA-F]{12}$/.test(raw)) {
    return 'must be 12 hex digits, no separators';
  }
  if (row.type === 'hex32' && !/^[0-9a-fA-F]{32}$/.test(raw)) {
    return 'must be 32 hex digits';
  }
  if (row.type === 'hexascii' && !/^([0-9a-fA-F]{2})+$/.test(raw)) {
    return 'must be hex, a whole number of bytes';
  }

  const m = S.META[row.name];
  if (m && m.options) {
    const allowed = m.options.split('|').map((p) => p.slice(0, p.indexOf('=')));
    if (!allowed.includes(raw)) return 'not one of: ' + allowed.join(', ');
  }
  return rangeBad(row, raw);
}

/*
 * Some keys hold the hex of an ASCII string — GPON_PLOAM_PASSWD keeps
 * "1234567890" as 31323334353637383930. Show the readable form, keeping the hex
 * alongside since that is what the device stores and what you would type back.
 *
 * Driven by the schema type, never sniffed: INT1 holds 2147483647, which is
 * valid hex decoding to '!GH6G', so a heuristic would mangle plain integers.
 * A PLOAM password is 10 arbitrary octets by standard and is not required to be
 * printable, so a value that is not stays hex.
 */
function hexAscii(hex) {
  if (!hex) return null;
  if (hex.length % 2 || /[^0-9a-fA-F]/.test(hex)) return null;
  let out = '';
  for (let i = 0; i < hex.length; i += 2) {
    const c = parseInt(hex.slice(i, i + 2), 16);
    if (c < 0x20 || c > 0x7e) return null;
    out += String.fromCharCode(c);
  }
  return out;
}

function validateInput(row, input) {
  /* Only mark what the user is actually proposing. A value the device already
     holds that falls outside its option list is shown as it is, not scolded. */
  const bad = EDITS.has(row.name) ? valueProblem(row, input.value) : null;
  input.classList.toggle('invalid', !!bad);
  input.title = bad || '';
  return !bad;
}

/* Every queued edit that the daemon would refuse, as [name, why] pairs. */
function invalidEdits() {
  const out = [];
  for (const [name, v] of EDITS) {
    const row = S.SCHEMA.find((r) => r.name === name);
    /* The page does not offer a key the firmware ignores; an edit to one can
       only be left over, and is refused rather than saved for nothing. */
    const unmet = row && dependsUnmet(row);
    const why = unmet ? 'ignored by the firmware unless ' + unmet.join(' and ')
      : row && valueProblem(row, v);
    if (why) out.push([name, why]);
  }
  return out;
}

export { rangeBad, valueProblem, hexAscii, validateInput, invalidEdits };
