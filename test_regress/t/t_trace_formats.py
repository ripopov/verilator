#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import os
import json
from pathlib import Path
import re

import vltest_bootstrap

test.scenarios('vlt', 'vltmt')
if not os.environ.get('VTR_INCLUDE') or not os.environ.get('VTR_LIBDIR'):
    test.skip('VTR_INCLUDE and VTR_LIBDIR are required for VTR runtime tests')

test.compile(verilator_flags2=[
    '--trace-vcd', '--trace-vtr', '--trace-structs', '--output-split', '20',
    '--output-split-ctrace', '10', '-Wno-UNUSEDSIGNAL', '-CFLAGS', '-std=c++20', '--exe',
    test.pli_filename
],
             make_top_shell=False,
             make_main=False,
             threads=2 if test.vltmt else 1,
             make_flags=['OBJCACHE=', "LDLIBS='-lvtr -llz4 -lzstd -lz'"])
for mode in ('vcd', 'vtr-inline', 'vtr-threaded'):
    test.execute(run_env='VTR_TRACE_MODE=' + mode,
                 logfile=test.obj_dir + '/formats-' + mode + '.log')

for suffix in ('', '.reopen'):
    prefix = Path(test.obj_dir) / ('formats.vcd' + suffix)
    expected = dict(line.split()
                    for line in Path(str(prefix) +
                                     '.values').read_text(encoding='utf-8').splitlines())
    for mode in ('vtr-inline', 'vtr-threaded'):
        captured = Path(test.obj_dir) / ('formats.' + mode + suffix)
        assert Path(str(prefix) + '.values').read_bytes() == Path(str(captured) +
                                                                  '.values').read_bytes()
        companion = json.loads(Path(str(captured) + '.vdb.json').read_text(encoding='utf-8'))
        binding = companion.pop('trace_binding')
        assert binding['prefix'] == 'TOP' and binding['signals']
        assert companion == json.loads(
            (Path(test.obj_dir) / ('V' + test.name + '.vdb.json')).read_text(encoding='utf-8'))
    code, time, actual = None, None, {}
    for line in prefix.read_text(encoding='utf-8').splitlines():
        match = re.search(r'\$var wire 95 (\S+) wide ', line)
        if match:
            code = match[1]
        elif line.startswith('#'):
            time = line[1:]
        elif code and line.startswith('b') and line.endswith(' ' + code):
            actual[time] = line[1:].split()[0].rjust(95, '0')
    assert actual == expected
test.passes()
