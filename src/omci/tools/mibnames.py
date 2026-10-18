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

# class id -> description shown for the entity. The plugin's own description
# is kept, except where it names a make (the classes above) or spells a G.988
# entity in a way the standard does not (ONT for ONU, "Ani-g"). Only
# descriptions are replaced here; names, ids and layout are untouched.
DESC = {
    2:     'ONU data',
    256:   'ONU-G',
    257:   'ONU2-G',
    263:   'ANI-G',
    264:   'UNI-G',
    243:   'Private queue map',
    255:   'Private ONU statistics',
    350:   'Private ME 350',
    370:   'Private ME 370',
    373:   'Private ME 373',
    65282: 'Private multicast tag',
    65294: 'Private SNTP',
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
