"""Names the committed OMCI model uses for entities whose stock plugins name
them after the OLT or ONT make they were written for.

Nothing public carries those makes, so the model is generated with a neutral
name chosen by class id, and by function where the attributes show one. The
stock name never appears in a committed file: mib-tables.py writes it to a
local sidecar (--stock-names), and mib-instances.py and mib-defaults.py read
that sidecar to match the labels of a stock `omcicli mib get all` dump, which
still use the stock names. Without the sidecar those few tables are reported
as "not in the model" and their rows are left out.

Only names change. Class ids, attributes, widths, flags and actions are read
from the plugin as before.
"""
import json

# class id -> (table name, short name, plugin file base or None to keep it).
# The plugin file is renamed only where the stock file name is itself the
# make; the others (mib_Me350.so ...) are already neutral.
GENERIC = {
    243:   ('PrivateQueueMap', 'PRIVQUEUEMAP', None),            # queue count, map, weights
    350:   ('Me350',           'ME350',         None),
    370:   ('Me370',           'ME370',         None),
    373:   ('Me373',           'ME373',         None),
    65282: ('PrivateMcastTag', 'PRIVMCASTTAG',  'PrivateMcastTag'),  # multicast tag strip
    65294: ('PrivateSntp',     'PRIVSNTP',      'PrivateSntp'),      # SNTP time zone and servers
}

# class id -> description shown for the entity. Every class G.988 defines
# carries the standard's own name for it, word for word; the plugins spell
# many differently ("MAC Bridge Port Configuration Data", "Priority Queue-G",
# ONT for ONU). G.988 leaves 240-255, 350-399 and 65280-65535 to vendors, so
# those have no official name: they keep the plugin's, with its casing and
# typos tidied, and a neutral one where it names a make (the classes above).
# Only descriptions are replaced here; names, ids and layout are untouched.
DESC = {
    # G.988
    2:     'ONU data',
    5:     'Cardholder',
    6:     'Circuit pack',
    7:     'Software image',
    11:    'Physical path termination point Ethernet UNI',
    24:    'Ethernet performance monitoring history data',
    45:    'MAC bridge service profile',
    46:    'MAC bridge configuration data',
    47:    'MAC bridge port configuration data',
    49:    'MAC bridge port filter table data',
    50:    'MAC bridge port bridge table data',
    52:    'MAC bridge port performance monitoring history data',
    78:    'VLAN tagging operation configuration data',
    79:    'MAC bridge port filter preassign table',
    83:    'Physical path termination point LCT UNI',
    84:    'VLAN tagging filter data',
    89:    'Ethernet performance monitoring history data 2',
    130:   'IEEE 802.1p mapper service profile',
    131:   'OLT-G',
    133:   'ONU power shedding',
    134:   'IP host config data',
    136:   'TCP/UDP config data',
    137:   'Network address',
    148:   'Authentication security method',
    157:   'Large string',
    158:   'ONU remote debug',
    171:   'Extended VLAN tagging operation configuration data',
    256:   'ONU-G',
    257:   'ONU2-G',
    262:   'T-CONT',
    263:   'ANI-G',
    264:   'UNI-G',
    266:   'GEM interworking termination point',
    267:   'GEM port performance monitoring history data',
    268:   'GEM port network CTP',
    272:   'GAL Ethernet profile',
    273:   'Threshold data 1',
    274:   'Threshold data 2',
    277:   'Priority queue',
    278:   'Traffic scheduler',
    280:   'Traffic descriptor',
    281:   'Multicast GEM interworking termination point',
    284:   'Pseudowire maintenance profile',
    287:   'OMCI',
    296:   'Ethernet performance monitoring history data 3',
    298:   'Dot1 rate limiter',
    307:   'Octet string',
    308:   'General purpose buffer',
    309:   'Multicast operations profile',
    310:   'Multicast subscriber config info',
    311:   'Multicast subscriber monitor',
    312:   'FEC performance monitoring history data',
    321:   'Ethernet frame performance monitoring history data downstream',
    322:   'Ethernet frame performance monitoring history data upstream',
    329:   'Virtual Ethernet interface point',
    330:   'Generic status portal',
    334:   'Ethernet frame extended PM',
    340:   'BBF TR-069 management server',
    341:   'GEM port network CTP performance monitoring history data',
    # vendor-specific: no official name
    240:   'ONU system management',
    242:   'ME 242',
    243:   'Private queue map',
    244:   'ONU self loop detect',
    248:   'WAN interface config',
    249:   'WAN interface config Deti',
    250:   'PPPoE interface',
    253:   'Loop detect',
    255:   'Private ONU statistics',
    350:   'Private ME 350',
    370:   'Private ME 370',
    373:   'Private ME 373',
    65282: 'Private multicast tag',
    65294: 'Private SNTP',
    65527: 'OLT location config data',
    65528: 'CTC loop detection',
    65531: 'Extended multicast operations profile',
    65534: 'Private T-CONT queue configuration',
    65535: 'Private VLAN configuration',
}

# Offsets of the handler slots of the per-ME vtable that carry a function
# name in the recovered model. The stock symbol names of a renamed entity
# are replaced by <TableName><Slot>: nothing reads them.
OPER_SLOTS = {'0': 'DrvCfg', '36': 'OperReset', '40': 'OperUpdate'}


def rename_entry(entry):
    """Apply GENERIC to one model entry in place. Returns (stock table name,
    new table name) when something was renamed, else None."""
    t = entry['table']
    if t['classId'] in DESC:
        t['desc'] = DESC[t['classId']]
    gen = GENERIC.get(t['classId'])
    if gen is None:
        return None
    stock = t['name']
    t['name'], t['shortName'] = gen[0], gen[1]
    if gen[2]:
        entry['plugin'] = 'mib_%s.so' % gen[2]
    for off in list(entry['oper']):          # keys are ints here, strings once in JSON
        kind = OPER_SLOTS.get(str(off))
        v = entry['oper'][off]
        if kind and isinstance(v, (list, tuple)) and len(v) == 2 and v[0] == 'fn':
            entry['oper'][off] = ['fn', gen[0] + kind]
    return stock, gen[0]


def write_stock_names(aliases, path):
    """aliases: stock table name -> new table name."""
    with open(path, 'w') as fh:
        json.dump({k.lower(): v for k, v in aliases.items()}, fh, indent=1, sort_keys=True)


def load_aliases(argv):
    """The sidecar named by `--stock-names FILE` in argv, else {}."""
    if '--stock-names' not in argv:
        return {}
    with open(argv[argv.index('--stock-names') + 1]) as fh:
        return json.load(fh)


def canonical(name, aliases):
    return aliases.get(name.lower(), name)
