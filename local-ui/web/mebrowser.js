/*
 * The MIB, unedited.
 *
 * Everything the OLT built, in whatever shape the firmware prints it. The
 * Services tab answers six questions; this one answers the rest, and it is the
 * page to reach for when a thread asks you to post `omcicli mib get 84` --
 * because that is exactly what it runs.
 *
 * Two views of the same dump, deliberately: a table when the parser could make
 * sense of it, and the bytes underneath when you want to be sure the table is
 * not lying to you. Nothing here decides what an attribute means.
 */

import { $, el } from './dom.js';
import { fetchOmci, fetchMe } from './omci.js';

/*
 * The table names `omcicli get tables` reported, taken off a stick once.
 *
 * Captured rather than queried, because asking costs the MIB service: that
 * command returns zero bytes and wedges the stock omci_app responder until it
 * is restarted. omcid answers it safely, but this page also runs against the
 * stock daemon, so the refusal stays. See scripts/fixtures/omci/_tables-names.txt.
 *
 * NOT the /lib/omci/mib_*.so filenames, which is what this list used to be:
 * 23 of the 81 differ (`AuthSecMethod` against `Authen_Sec_Method`) and some
 * runtime names contain spaces. A name omcicli does not recognise produces
 * empty output rather than an error, so the filename form failed silently and
 * looked like an empty table.
 *
 * The device is still asked at load time and that answer wins -- only the
 * running stack knows what it registered. This is the list until it replies,
 * and for the case where it cannot.
 */
const TABLES = [
  'Anig', 'AuthSecMethod', 'Cardholder', 'CircuitPack',
  'CTCOnuOnuLoopDetection', 'Dot1RateLimiter', 'EthExtPmData', 'EthPmData2',
  'EthPmData3', 'EthPmDataDs', 'EthPmDataUs', 'EthPmHistoryData', 'EthUni',
  'ExtendedMcastOperProf', 'ExtendedOnuG', 'ExtIpHostCfgData',
  'ExtVlanTagOperCfgData', 'FecPmhd', 'GalEthProf', 'GemIwTp', 'GemPortCtp',
  'GemPortPmhd', 'GeneralPurposeBuffer', 'GenericStatusPortal', 'GpncPmhd',
  'HSQDefault', 'HW ME350', 'HW ME370', 'HW ME373', 'IpHostCfgData',
  'LargeString', 'LargeString', 'LctUni', 'LoIdAuth', 'LoopDetect',
  'MacBridgePortFilterPreassign', 'MacBridgePortFilterTable',
  'MacBridgePortPmMonitorHistoryData', 'MacBriPortBriTblData',
  'MacBriPortCfgData', 'MacBriServProf', 'Map8021pServProf', 'McastOperProf',
  'McastSubConfInfo', 'McastSubMonitor', 'Me242', 'MultiGemIwTp',
  'NetworkAddress', 'OltG', 'OLTLocationConfigData', 'Omci', 'Ont2g',
  'OntData', 'Ontg', 'OntSelfLoopDetect', 'OntSystemMgmt', 'OnuCapability',
  'OnuPwrShedding', 'OnuRemoteDebug', 'PriQ', 'PrivateTellionOntStatistics',
  'PrivateTqCfg', 'PrivateVlanCfg', 'PseudowireMaintenanceProfile',
  'Scheduler', 'SWImage', 'Tcont', 'TcpUdpCfgData', 'ThresholdData1',
  'ThresholdData2', 'TR069ManageServer', 'TrafficDescriptor', 'Unig', 'VEIP',
  'VlanTagFilterData', 'VlanTagOpCfgData', 'WANConfigCata',
  'WANConfigCataDeti', 'ZTE ME243', 'ZteMcastTag', 'ZteSntp'
];

/*
 * Class ids confirmed against this device: from the captures in
 * scripts/fixtures/omci/, and the G.988 class each is defined as in the
 * README's "The MIB tabs" section. The rest of TABLES has no id confirmed
 * against this hardware, so the picker offers the name alone rather than a
 * guessed number -- omcicli takes a table name just as well as a class id.
 */
const CLASS_IDS = {
  SWImage: 7, EthUni: 11, MacBriPortCfgData: 47, OltG: 131,
  ExtVlanTagOperCfgData: 171, Tcont: 262, GemIwTp: 266, GemPortCtp: 268,
  VlanTagFilterData: 84, VEIP: 329,
};

const DUMPS = [
  ['conn', 'Data path connections'],
  ['srvflow', 'Data path service flows'],
  ['qmap', 'T-CONT queue mapping'],
  ['tasks', 'OMCI tasks'],
];

let wired = false;

function renderTable(host, dump) {
  if (!dump.instances.length) {
    /* Not "the OLT created none": omcid has answered 0 rows for entities
       the ONU always has, so an empty answer is only what the MIB said. */
    host.append(el('p', 'hint', dump.rows === 0
      ? 'The MIB answered 0 rows for this table.'
      : 'No instances in this dump.'));
    return;
  }
  for (const inst of dump.instances) {
    const box = el('section', 'me-inst');

    box.append(el('h4', '', 'Entity ' + (inst.id || '(unnamed)')));
    const dl = el('dl', 'me-kv');
    for (const [k, v] of inst.attrs) {
      if (/^entity\s*id$/i.test(k)) continue;
      dl.append(el('dt', '', k), el('dd', '', v));
    }
    if (inst.attrs.length > 1) box.append(dl);

    for (const g of inst.groups) {
      box.append(el('h5', '', g.label));
      if (!g.attrs.length) continue;
      const gdl = el('dl', 'me-kv');
      for (const [k, v] of g.attrs) gdl.append(el('dt', '', k), el('dd', '', v));
      box.append(gdl);
    }
    host.append(box);
  }
}

function show(dump, asked) {
  const host = $('#me-out');

  host.textContent = '';

  if (!dump.ok && dump.error) {
    host.append(el('p', 'me-note bad', dump.error));
    if (!dump.raw) return;
  }

  host.append(el('p', 'me-src', dump.name
    ? asked + ', which the device calls ' + dump.name
    : asked));

  if (dump.truncated)
    host.append(el('p', 'me-note bad', 'Truncated: this dump is longer than the '
      + 'daemon will hold. Ask for a single entity to see all of it.'));

  renderTable(host, dump);

  const det = el('details', 'me-raw');
  det.append(el('summary', '', 'Raw output'));
  det.append(el('pre', '', dump.raw || '(no output)'));
  host.append(det);
}

async function load() {
  /* Either control selects what to query: a typed class id wins when
     present, otherwise whatever the dropdown has picked. */
  const typed = String($('#me-classid').value || '').trim();
  const me = typed || String($('#me-select').value || '').trim();
  const entity = String($('#me-entity').value || '').trim();

  if (!me) return;
  $('#me-out').textContent = 'Reading…';
  show(await fetchMe(me, entity), 'ME ' + me + (entity ? ' entity ' + entity : ''));
}

async function loadDump(what, label) {
  $('#me-out').textContent = 'Reading…';
  show(await fetchOmci({ cmd: 'dump', what }), label);
}

function renderMeBrowser() {
  if (wired) return;
  wired = true;

  /* A select, not a datalist: these are every table this image can register,
     shown as "Name (class id)" where the id is confirmed and by name alone
     otherwise. The class-id field beside it is the escape hatch for a class
     a thread quotes that is not in this list, or that this base has not
     registered anything under -- either control queries the same route. */
  const sel = $('#me-select');
  for (const t of TABLES) {
    const id = CLASS_IDS[t];
    const o = el('option', '', id ? `${t} (${id})` : t);
    o.value = id || t;
    sel.append(o);
  }
  sel.value = CLASS_IDS.VlanTagFilterData;

  const bar = $('#me-dumps');
  for (const [what, label] of DUMPS) {
    const b = el('button', 'chip', label);
    b.type = 'button';
    b.onclick = () => loadDump(what, label);
    bar.append(b);
  }

  $('#me-go').onclick = load;
  $('#me-classid').onkeydown = (e) => { if (e.key === 'Enter') load(); };
  $('#me-entity').onkeydown = (e) => { if (e.key === 'Enter') load(); };

  /*
   * The device is deliberately NOT asked for its table list.
   * `omcicli get tables` returns nothing and leaves the stock MIB service unable
   * to answer anything until omci_app is restarted -- so this tab used to disable
   * the diagnostics it exists to show, just by being opened. The names below
   * came off a stick once and change only with the base.
   */
  $('#me-tables').textContent = TABLES.join('\n');

  load();
}

export { renderMeBrowser };
