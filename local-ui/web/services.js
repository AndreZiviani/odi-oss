/*
 * What the OLT actually built on this ONU, in plain language.
 *
 * The config page shows what we ASKED for. This one shows what the line
 * BUILT -- and when those disagree, the line is right. Nearly every "state is
 * O5 but nothing passes" report on this hardware is settled by two of these
 * tables, so they get sentences rather than a hex dump; the dump is one tab
 * over.
 *
 * The rule this page is written to: never invent a meaning. Where an attribute
 * has an unambiguous reading -- a VLAN id is a VLAN id -- it is spelled out.
 * Where it does not, the value is shown as it came and the reader is pointed at
 * the raw view, because a confident wrong explanation of a MIB attribute is
 * worse than no explanation at all. That is why there is no table of
 * forwarding-operation codes here: the VIDs in the filter answer the question
 * people actually ask, and the operation byte is shown unread.
 */

import { $, el, get } from './dom.js';
import { fetchMe, attr, unavailable } from './omci.js';

/*
 * The six tables worth a sentence, in the order someone diagnosing a dead link
 * would want them. Class ids, not names: the number form is the one seen
 * working on this firmware in the field. Each card prints the table name that
 * came back, so asking for the wrong id shows up as a wrong name rather than a
 * mislabelled card.
 */
const CARDS = [
  { me: '7',   title: 'Software version the OLT sees',  render: renderSwImage, onu: true },
  { me: '131', title: 'The OLT at the other end',       render: renderOlt, onu: true },
  { me: '84',  title: 'VLANs this line allows',         render: renderVlanFilter },
  { me: '171', title: 'VLAN translation',               render: renderExtVlan },
  { me: '47',  title: 'How this line is bridged',       render: renderBridge },
  { me: '262', title: 'Upstream containers (T-CONT)',   render: renderIds, onu: true,
    key: 'AllocID', what: 'allocation id', unassigned: [255, 65535] },
  { me: '268', title: 'GEM ports',                      render: renderIds, key: 'PortID', what: 'GEM port id' },
];
/*
 * `onu: true` marks the entities the ONU creates for itself at MIB reset
 * (G.988): a software image, the OLT-G, the T-CONTs. An empty read of one of
 * those is never a fact about the line -- they always exist -- so it is shown
 * as a read that came back incomplete, not as "none". odi-oss omcid has
 * answered `0 rows` for them, and the page used to relay that as the OLT
 * having created nothing.
 */

/*
 * Vendor codes are four ASCII characters and the OLT reports them as a 32-bit
 * hex word. Only vendors seen on these threads are named; anything else shows
 * the code, which is still the useful half.
 */
const VENDORS = {
  ALCL: 'Nokia (Alcatel-Lucent)',
  HWTC: 'Huawei',
  ZTEG: 'ZTE',
  FHTT: 'FiberHome',
};

/* `PRI 8,VID 35, TPID 0, EthType 0x00` -> { PRI: '8', VID: '35', ... } */
function fields(v) {
  const out = {};
  for (const part of String(v || '').split(',')) {
    const m = /^\s*(\S+)\s+(\S+)\s*$/.exec(part);
    if (m) out[m[1]] = m[2];
  }
  return out;
}

/*
 * G.988 fills unused filter fields with the maximum the field can hold: a VLAN
 * id is 12 bits, so 4096 cannot be one and means "do not filter on this"; a
 * priority is 3 bits, so 15 means the same. Both are printed by the firmware as
 * ordinary numbers, and reading them as real values is how a rule that matches
 * everything gets described as a rule that matches VLAN 4096.
 */
const vidText = (v) => (v === '4096' ? 'any' : v);
const priText = (v) => (v === '15' ? 'any' : v);

const hexToAscii = (h) => {
  const s = String(h || '').replace(/^0x/i, '');
  let out = '';
  for (let i = 0; i + 1 < s.length; i += 2) {
    const c = parseInt(s.slice(i, i + 2), 16);
    if (c < 0x20 || c > 0x7e) return '';
    out += String.fromCharCode(c);
  }
  return out;
};

function note(text, cls) {
  return el('p', 'me-note' + (cls ? ' ' + cls : ''), text);
}

/* --- the six renderers --------------------------------------------------- */

/*
 * What the stick believes its versions are, from the U-Boot environment. Read
 * alongside the MIB so the software-image card can compare the two: ME 7 is
 * what the OLT was TOLD, and these are what the partitions actually hold.
 */
let ENV = {};

function renderSwImage(box, dump) {
  const dl = el('dl', 'me-kv');
  for (const inst of dump.instances) {
    const ver = attr(inst, 'Version') || '(empty)';
    /* `Active` and `Committed`, not `IsActive`/`IsCommitted` -- read off a
       stick, after the preview stub had guessed the other spelling and this
       card silently showed no tags at all. */
    const act = Number(attr(inst, 'Active'));
    const com = Number(attr(inst, 'Committed'));
    const tags = [];
    if (act === 1) tags.push('running');
    if (com === 1) tags.push('committed');
    dl.append(el('dt', '', 'Image ' + (inst.id || '?')),
              el('dd', '', ver + (tags.length ? ' (' + tags.join(', ') + ')' : '')));
  }
  box.append(dl);
  box.append(note('The only version string your ISP sees. Check it here after every flash '
    + 'if it has to match the ONU you replaced.'));

  /*
   * The comparison that makes this card worth having. ME 7 is what the OLT was
   * told; sw_version<n> is what U-Boot records for the partition. On this
   * image omcid answers "0.0.0" unless the ONU identity switch is on, and then
   * OMCI_SW_VER1 for image 0 and OMCI_SW_VER2 for image 1 -- so a difference
   * is usually that switch being off, and saying which key sets it is the
   * point. On the stock firmware the same question ("my OMCI software version
   * resets at every boot", Anime4000/RTL960x#30) has another answer, because
   * the stock image rewrites those keys itself.
   */
  const active = ENV.sw_active;
  const held = ENV['sw_version' + active];
  const told = dump.instances.map((i) => attr(i, 'Version')).filter(Boolean);

  if (!held || !told.length) return;
  if (told.some((v) => v === held)) {
    box.append(note('Matches what partition ' + active + ' holds.'));
    return;
  }
  const key = active === '1' ? 'OMCI_SW_VER2' : 'OMCI_SW_VER1';
  box.append(note('Partition ' + active + ' holds “' + held + '”, which is not '
    + 'what the OLT was told. That is expected here: omcid reports 0.0.0 '
    + 'unless the ONU identity switch is on (Config), and then ' + key
    + ' for this image. Set the key, turn the switch on and apply to choose '
    + 'the reported string -- a change the OLT sees, so only on a line that '
    + 'checks it.'));
}

function renderOlt(box, dump) {
  const inst = dump.instances[0];
  const hex = inst ? attr(inst, 'OltVendorId') : null;
  const code = hexToAscii(hex);
  const dl = el('dl', 'me-kv');

  dl.append(el('dt', '', 'Vendor'),
            el('dd', '', code ? (VENDORS[code] ? code + ' — ' + VENDORS[code] : code)
                              : String(hex || 'not reported')));
  if (inst) {
    for (const [k, v] of inst.attrs) {
      if (/^oltvendorid$/i.test(k) || /^entity\s*id$/i.test(k)) continue;
      dl.append(el('dt', '', k), el('dd', '', v));
    }
  }
  box.append(dl);
}

function renderVlanFilter(box, dump) {
  const vids = [];

  for (const inst of dump.instances) {
    for (const [k, v] of inst.attrs) {
      if (!/^FilterTbl\[/i.test(k)) continue;
      const f = fields(v);
      if (f.VID !== undefined) vids.push(f.VID);
    }
  }

  if (!vids.length) {
    box.append(note('No tag filters are installed.'));
    return;
  }

  box.append(el('p', 'me-lead', vids.length === 1
    ? 'The OLT allows one VLAN on this line: ' + vids[0] + '.'
    : 'The OLT allows these VLANs on this line: ' + vids.join(', ') + '.'));
  box.append(note('Tag a VLAN outside that list and the ONU still reaches O5 '
    + 'while the traffic is discarded — which looks exactly like a dead line. '
    + 'The forwarding-operation byte that decides how strict each filter is '
    + 'shows unread in the raw view.'));
}

function renderExtVlan(box, dump) {
  const rules = [];

  for (const inst of dump.instances) {
    for (const g of inst.groups) {
      if (!/^INDEX\b/i.test(g.label)) continue;
      const row = {};
      for (const [k, v] of g.attrs) row[k.toLowerCase()] = fields(v);
      rules.push(row);
    }
  }

  if (!rules.length) {
    box.append(note('No VLAN translation rules are installed: the OLT is not '
      + 'rewriting tags on this line.'));
    return;
  }

  const list = el('div', 'me-rules');
  let translated = null;

  for (const r of rules) {
    const fi = r['filter inner'] || {};
    const ti = r['treatment inner'] || {};
    const from = vidText(fi.VID);
    /*
       A treatment whose priority is 15 adds no tag at all. PRI is a 3-bit
       field, so 15 is out of range and is the same "unspecified" sentinel
       already read that way in a filter -- and on a live stick most rows carry
       exactly that, which this rendered as "any VLAN -> VLAN 0". A VLAN id
       nobody set, printed as if the OLT had set it, is the sort of confident
       wrong answer this page exists to avoid.
    */
    const adds = priText(ti.PRI) !== 'any' || (ti.VID !== undefined && ti.VID !== '0');
    const to = adds ? vidText(ti.VID) : null;
    const line = el('div', 'me-rule');

    line.append(el('span', 'me-from', from === 'any' ? 'any VLAN' : 'VLAN ' + from));
    line.append(el('span', 'me-arrow', '→'));
    line.append(el('span', 'me-to', to === null ? 'no tag added'
                                  : to === 'any' ? 'unchanged' : 'VLAN ' + to));
    if (fi.PRI && priText(fi.PRI) !== 'any')
      line.append(el('span', 'me-pri', 'priority ' + fi.PRI));
    list.append(line);

    if (from !== 'any' && to && to !== 'any' && from !== to && translated === null)
      translated = { from, to };
  }
  box.append(list);

  if (translated) {
    box.append(el('p', 'me-lead', 'The OLT expects VLAN ' + translated.to
      + ' on the fibre and calls it VLAN ' + translated.from + ' on your side.'));
    box.append(note('Which of the two your router should tag is the most common '
      + 'thing to get wrong here. An ONU that applies this rule wants '
      + translated.from + '. Users of this Realtek chipset report it does not '
      + 'apply the rule, and that ' + translated.to + ' is what works. If one '
      + 'carries no traffic, try the other.'));
  }
}

/*
 * What the OLT attached to the bridge.
 *
 * This can differ between lines and it is structural rather than a state
 * flag: one OLT's bridge can carry the physical Ethernet UNI *and* the
 * VEIP, another's just the VEIP alone. It is the question behind most of
 * the VEIP threads upstream.
 *
 * The TP type is resolved by POINTER, not by a table of type numbers: each
 * port's TPPointer is the entity id of a real managed entity on the same
 * stick -- 0x0101 is the EthUni that ME 11 reports, 0x0601 the VEIP that ME
 * 329 reports, and a bridge's `TPType 3` pointers are exactly the entity
 * ids of its GEM interworking TPs. So the naming below is checked against
 * the device rather than recalled from a specification.
 */
const TP_TYPES = {
  1:  'physical Ethernet UNI',
  3:  'GEM interworking TP',
  11: 'VEIP',
};

function renderBridge(box, dump) {
  const ports = [];

  for (const inst of dump.instances) {
    const type = attr(inst, 'TPType');
    const ptr = attr(inst, 'TPPointer');

    if (type === null) continue;
    ports.push({ port: attr(inst, 'PortNum'), bridge: attr(inst, 'BridgeIdPtr'),
                 type: Number(type), ptr });
  }
  if (!ports.length) {
    box.append(note('No bridge ports. The OLT has not built a data path.'));
    return;
  }

  const named = ports.filter((p) => TP_TYPES[p.type]);
  const uni = named.some((p) => p.type === 1);
  const veip = named.some((p) => p.type === 11);

  if (uni || veip) {
    box.append(el('p', 'me-lead', uni && veip
      ? 'The bridge carries both the physical Ethernet UNI and the VEIP.'
      : uni ? 'The bridge carries the physical Ethernet UNI.'
            : 'The bridge carries the VEIP, not the physical port.'));
  }

  const t = el('table', 'data');
  for (const p of ports) {
    const tr = el('tr');

    tr.append(el('td', 'mono', 'port ' + (p.port === null ? '?' : Number(p.port))));
    tr.append(el('td', null, TP_TYPES[p.type] || 'type ' + p.type));
    tr.append(el('td', 'mono', p.ptr || ''));
    t.append(tr);
  }
  box.append(t);
  box.append(note('Each row is a bridge port and what the OLT attached to it. '
    + 'The pointer is the entity id of a managed entity on this stick, so it '
    + 'can be looked up on the MIB tab; a type shown as a bare number is one '
    + 'no capture here has pinned down.'));
}

/* T-CONTs and GEM ports: what exists, not what each one is set to. The
   identifier a support thread asks for is the allocation or port id, not the
   entity id, so that is what is listed when the dump carries it. */
function renderIds(box, dump, card) {
  const vals = dump.instances.map((i) => {
    const v = card && card.key ? attr(i, card.key) : null;
    return v !== null && v !== '' && Number.isFinite(Number(v)) ? Number(v) : null;
  });
  /*
     The ONU creates every T-CONT it has at MIB reset, with no Alloc-ID; the
     OLT assigns one to each T-CONT it uses. G.988 (9.2.2) gives 255 as "no
     Alloc-ID assigned" on G.984 GPON, and 65535 on the later PON families.
     Counting those as provisioned turned five T-CONTs into sixteen.
  */
  const free = new Set(card && card.unassigned || []);
  const used = [];
  let spare = 0;
  dump.instances.forEach((inst, i) => {
    if (vals[i] === null) used.push(inst.id);
    else if (free.has(vals[i])) spare++;
    else used.push(String(vals[i]));
  });
  const by = card && card.key && vals.some((v) => v !== null) ? ', by ' + card.what : '';
  if (!used.length) {
    box.append(el('p', 'me-lead', 'None assigned'));
  } else {
    box.append(el('p', 'me-lead', used.length + (free.size ? ' assigned' : ' provisioned')
      + by + ': ' + used.join(', ')));
  }
  if (spare) {
    box.append(note(spare + ' unassigned: created by the ONU, not given an '
      + card.what + ' by the OLT.'));
  }
}

/* --- the page ------------------------------------------------------------ */

let loaded = false;
let RUN = 0;

/*
 * A card is always in one of four states, and each says only what it knows:
 *   reading          the request is out
 *   could not read   the daemon failed, answered nothing, or answered 0 rows
 *                    for an entity the ONU always has
 *   none             a good read of a table the OLT fills, holding nothing
 *   the rows         rendered in sentences
 */
async function renderServices(force) {
  const host = $('#services-cards');

  if (loaded && !force) return;
  loaded = true;
  const run = ++RUN;
  host.textContent = '';

  const slots = CARDS.map((c) => {
    const card = el('section', 'me-card reading');
    card.append(el('h3', '', c.title));
    const src = el('p', 'me-src', 'ME ' + c.me);
    const body = el('div', 'me-body');
    body.append(el('p', 'me-state', 'Reading\u2026'));
    card.append(src, body);
    card.setAttribute('aria-busy', 'true');
    host.append(card);
    return { c, card, src, body };
  });

  /* Never let the firmware read take the page down: it is context for one
     card, and the other six are worth rendering without it. */
  try { ENV = (await get('/api/firmware')).env || {}; } catch (e) { ENV = {}; }

  for (const { c, card, src, body } of slots) {
    const dump = await fetchMe(c.me);
    if (run !== RUN) return;
    card.classList.remove('reading');
    card.removeAttribute('aria-busy');
    body.textContent = '';
    /* The name the device printed, not the one we asked for. */
    if (dump.name) src.textContent = 'ME ' + c.me + ', ' + dump.name;

    const why = unavailable(dump) || (c.onu && !dump.instances.length
      ? 'The MIB answered with no rows, but the ONU always creates this entity itself, '
        + 'so the read is incomplete. It says nothing about the line.'
      : null);

    if (why) {
      card.classList.add('unread');
      body.append(el('p', 'me-state', 'Could not read'), note(why));
    } else if (!dump.instances.length && c.render === renderIds) {
      card.classList.add('empty');
      body.append(el('p', 'me-state', 'None'), note('The MIB holds none: the OLT has not provisioned any.'));
    } else {
      c.render(body, dump, c);
    }

    if (dump.truncated)
      body.append(note('This dump was longer than the daemon will hold; what '
        + 'you see is the start of it.', 'bad'));

    const det = el('details', 'me-raw');
    det.append(el('summary', '', 'Raw'));
    det.append(el('pre', '', dump.raw || '(no output)'));
    body.append(det);
  }
}

export { renderServices };
