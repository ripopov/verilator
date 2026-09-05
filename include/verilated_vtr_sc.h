// -*- mode: C++; c-file-style: "cc-mode" -*-
//=============================================================================
//
// Code available from: https://verilator.org
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2001-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//=============================================================================
///
/// \file
/// \brief Verilated tracing in VTR format for SystemC header
///
/// User wrapper code should use this header when creating VTR SystemC
/// traces.
///
//=============================================================================

#ifndef VERILATOR_VERILATED_VTR_SC_H_
#define VERILATOR_VERILATED_VTR_SC_H_

#include "verilatedos.h"

#include "verilated_sc_trace.h"
#include "verilated_vtr_c.h"

//=============================================================================
// VerilatedVtrSc
///
/// Class representing a Verilator-friendly VTR trace format registered
/// with the SystemC simulation kernel, just like a SystemC-documented
/// trace format.

class VerilatedVtrSc final : VerilatedScTraceBase, public VerilatedVtrC {
    // CONSTRUCTORS
    VL_UNCOPYABLE(VerilatedVtrSc);

public:
    /// Construct a SC trace object, and register with the SystemC kernel
    VerilatedVtrSc() {
        spTrace()->set_time_unit(VerilatedScTraceBase::getScTimeUnit());
        spTrace()->set_time_resolution(VerilatedScTraceBase::getScTimeResolution());
    }

    // METHODS
    // Override VerilatedVtrC. Must be called after starting simulation.
    void open(const char* filename) override VL_MT_SAFE {
        VerilatedScTraceBase::checkScElaborationDone();
        VerilatedVtrC::open(filename);
    }

    // METHODS - for SC kernel
    // Called from SystemC kernel
    void cycle() override { VerilatedVtrC::dump(sc_core::sc_time_stamp().to_double()); }
};

#endif  // Guard
