#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import hashlib
import json
from pathlib import Path
import vltest_bootstrap

test.scenarios('vlt')
test.compile(verilator_flags2=['--trace-vtr', '-Wno-UNUSEDSIGNAL'],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False)

path = Path(test.obj_dir) / ('V' + test.name + '.vdb.json')
db = json.loads(path.read_text())
assert db['format'] == 'vtr-rtl-vdb' and db['version'] == 2
assert db['top'] == 't'
identity = db.pop('design_id')
index = db.pop('source_index')
assert identity == hashlib.sha256(json.dumps(db, separators=(',', ':')).encode()).hexdigest()
assert index['producer'].startswith('slang ')
assert index['classes'][0] == 'keyword' and index['modifiers'][0] == 'declaration'
source = next(f for f in index['files'] if f['path'].endswith('t_trace_vdb.v'))
assert len(source['tokens']) % 5 == 0 and len(source['tokens']) > 100
assert len(source['declarations']) % 4 == 0 and source['declarations']
assert {'t', 'stage'} <= set(index['definitions'])
instances = {i['path']: i for i in db['instances']}
assert set(instances) == {'t', 't.u', 't.lanes[0].v', 't.lanes[1].v'}
assert instances['t.u']['definition'] == 'stage'
assert len(instances['t.u']['ports']) == 6
assert any(p['name'] == 'spare' for p in instances['t.u']['ports'])
assert db['symbols']['t.u.q']['type']['width'] == 15
assert db['symbols']['t.lanes[1].v.q']['type']['width'] == 7
assert db['symbols']['t.u.W']['value'].endswith("'shf")
assert db['symbols']['t.u.q']['source']['file'].endswith('t_trace_vdb.v')
assert all(s['source']['line'] > 0 and s['source']['column'] > 0 for s in db['symbols'].values())
seq = [p for p in db['processes'] if p['mode'] == 'seq']
assert len(seq) == 3
assert [e['edge'] for e in seq[0]['events']] == ['PosEdge', 'NegEdge']
assert any(p['reads'] == ['t.a', 't.b', 't.sel'] and 'Unsupported' in str(p['body'])
           for p in db['processes'])
assert all(i['parent'] == 't' for i in instances.values() if i['path'] != 't')
assert any(c['port'] == 't.u.q' and c['expression']['symbol'] == 't.q' for c in db['connections'])
assert not any(c['port'].endswith('.spare') for c in db['connections'])

test.passes()
