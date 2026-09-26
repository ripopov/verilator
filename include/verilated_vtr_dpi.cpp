// -*- mode: C++; c-file-style: "cc-mode" -*-
//=============================================================================
//
// Code available from: https://verilator.org
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//=============================================================================
///
/// \file
/// \brief The vtr_trace SystemVerilog package over the Verilator VTR sink
///
/// The package (include/vtr/vtr_trace.sv) is parsed automatically with
/// --trace-vtr; its DPI bodies (include/vtr/vtr_trace_dpi.hpp) are written
/// against a sink interface that this file implements over VerilatedVtr.
/// Both files are installed from the VTR revision the fork was built with.
///
//=============================================================================

#include "verilated.h"
#include "verilated_vtr_c.h"

#include "vtr/vtr_trace_dpi.hpp"

#include <cmath>
#include <map>
#include <memory>
#include <mutex>

namespace {

// A simulation context as the package sees it
class ContextHost final : public vtr_trace::Host {
    VerilatedContext* const m_contextp;

public:
    explicit ContextHost(VerilatedContext* contextp)
        : m_contextp{contextp} {}
    uint64_t now() override { return m_contextp->time(); }
    int timePrecision() override { return m_contextp->timeprecision(); }
    // $root is the model: DPI scope names and the trace both start with its name
    std::string rootPath() override {
        const VerilatedScope* const scopep = static_cast<const VerilatedScope*>(svGetScope());
        return scopep ? scopep->symsp()->name() : "TOP";
    }
};

struct ContextRuntime final {
    ContextHost host;
    vtr_trace::Runtime runtime;
    explicit ContextRuntime(VerilatedContext* contextp)
        : host{contextp}
        , runtime{host} {}
};

std::mutex s_mutex;  // Guards s_runtimes
std::map<const VerilatedContext*, std::unique_ptr<ContextRuntime>> s_runtimes;

vtr_trace::Runtime& runtimeOf(VerilatedContext* contextp) {
    const std::lock_guard<std::mutex> lock{s_mutex};
    std::unique_ptr<ContextRuntime>& entryp = s_runtimes[contextp];
    if (!entryp) entryp.reset(new ContextRuntime{contextp});
    return entryp->runtime;
}

}  // namespace

vtr_trace::Runtime& vtr_trace::runtime() {
    // Every package call lands here: remember the calling thread's last context, so the
    // global map and its mutex are only consulted when that context changes.
    // Runtimes are never destroyed, so the pointer stays valid.
    static thread_local const VerilatedContext* t_contextp = nullptr;
    static thread_local vtr_trace::Runtime* t_runtimep = nullptr;
    VerilatedContext* const contextp = Verilated::threadContextp();
    if (VL_UNLIKELY(contextp != t_contextp)) {
        t_runtimep = &runtimeOf(contextp);
        t_contextp = contextp;
    }
    return *t_runtimep;
}

//=============================================================================
// VerilatedVtrPackageSink: the open file as the package sees it

class VerilatedVtrPackageSink final : public vtr_trace::Sink {
    VerilatedVtr& m_trace;

public:
    VerilatedContext* const m_contextp;
    VerilatedVtrPackageSink(VerilatedVtr& trace, VerilatedContext* contextp)
        : m_trace{trace}
        , m_contextp{contextp} {}
    vtr_writer* writer() override { return m_trace.m_vtr; }
    int timescale() override {
        return static_cast<int>(std::lround(std::log10(m_trace.timeRes())));
    }
    uint32_t scopeNode(const std::string& path) override { return m_trace.scopeNode(path); }
    void warn(const std::string& text) override { m_trace.warn(text); }
};

void VerilatedVtr::packageOpened() {
    if (m_packagep || !m_vtr) return;
    m_packagep = new VerilatedVtrPackageSink{*this, m_logContextp};
    runtimeOf(m_logContextp).opened(*m_packagep);
}

void VerilatedVtr::packageClosing() {
    if (!m_packagep) return;
    runtimeOf(m_packagep->m_contextp).closing();
    VL_DO_CLEAR(delete m_packagep, m_packagep = nullptr);
}
