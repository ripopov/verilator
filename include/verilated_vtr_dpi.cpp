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
/// The vtr_trace package's DPI bodies use the sink implemented here over
/// VerilatedVtr. VTR_INCLUDE selects the installed SDK headers.
///
//=============================================================================

#include "verilated.h"
#include "verilated_vtr_c.h"

#include "vtr_trace_dpi.h"

#include <cmath>
#include <memory>

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

}  // namespace

class VerilatedVtrRuntime final : public VerilatedVirtualBase {
    ContextHost m_host;
    vtr_trace::Runtime m_runtime;
    explicit VerilatedVtrRuntime(VerilatedContext* contextp)
        : m_host{contextp}
        , m_runtime{m_host} {}

public:
    static vtr_trace::Runtime& of(VerilatedContext* contextp) {
        auto& owner = contextp->m_vtrRuntimep;
        if (!owner) owner.reset(new VerilatedVtrRuntime{contextp});
        return static_cast<VerilatedVtrRuntime*>(owner.get())->m_runtime;
    }
    static void abandon(VerilatedContext* contextp) noexcept {
        if (contextp->m_vtrRuntimep)
            static_cast<VerilatedVtrRuntime*>(contextp->m_vtrRuntimep.get())->m_runtime.abandon();
    }
};

vtr_trace::Runtime& vtr_trace::runtime() {
    return VerilatedVtrRuntime::of(Verilated::threadContextp());
}

void vtr_trace::failure(const char* message) {
    VL_FATAL_MT(__FILE__, __LINE__, "", message);
    std::abort();
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
    vtr::Writer& writer() override { return *m_trace.m_vtr; }
    void enter(uint64_t time) override { m_trace.enterRecording(time); }
    void leave() noexcept override { m_trace.leaveRecording(); }
    int timescale() override {
        return static_cast<int>(std::lround(std::log10(m_trace.timeRes())));
    }
    std::optional<vtr::ScopeId> scopeNode(const std::string& path) override {
        return m_trace.scopeNode(path);
    }
    void warn(const std::string& text) override { m_trace.warn(text); }
};

void VerilatedVtr::packageOpened() {
    if (m_packagep || !m_vtr) return;
    m_packagep = new VerilatedVtrPackageSink{*this, m_logContextp};
    VerilatedVtrRuntime::of(m_logContextp).opened(*m_packagep);
}

void VerilatedVtr::packageClosing() {
    if (!m_packagep) return;
    VerilatedVtrRuntime::of(m_packagep->m_contextp).closing();
    VL_DO_CLEAR(delete m_packagep, m_packagep = nullptr);
}

void VerilatedVtr::packageAbandon() noexcept {
    if (!m_packagep) return;
    VerilatedVtrRuntime::abandon(m_packagep->m_contextp);
    VL_DO_CLEAR(delete m_packagep, m_packagep = nullptr);
}
