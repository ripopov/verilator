#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2024 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('vlt')
test.compile(verilator_flags2=['--trace-vtr', '--assert'],
             verilator_make_gmake=False, make_main=False, make_top_shell=False)
files = test.glob_some(test.obj_dir + '/' + test.vm_prefix + '*.cpp')
for level in (2, 3, 4, 5):
    test.file_grep_any(files, r'VL_LOG_WRITEF_NX\(' + str(level) + r',')
test.file_grep_any(files, r'VL_FLOG_WRITEF_NX\(2,')
test.file_grep_any(files, r'VL_LOG_WRITEF_NX\(2,\s*"first')
test.file_grep_any(files, r'VL_LOG_WRITEF_NX\(2,\s*"second')
test.passes()
