// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include "verilated.h"
#include "verilated_vcd_c.h"
#include "verilated_vtr_c.h"

#include "vtr_reader.h"

#include VM_PREFIX_INCLUDE

#include <array>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

template <class Trace>
void capture(Trace& trace, VM_PREFIX& model, VerilatedContext& context, const std::string& prefix,
             const std::string& mode) {
    const uint64_t begin = context.time();
    trace.open(prefix.c_str());
    if (!trace.isOpen()) vl_fatal(__FILE__, __LINE__, "", "Trace did not open");
    std::ofstream expected{prefix + ".values"};
    std::vector<std::array<uint32_t, 3>> values;
    for (unsigned step = 0; step < 257; ++step) {
        const uint64_t time = context.time();
        model.clk = time & 1;
        model.value = ((time * 29) ^ (time >> 2)) & 127;
        model.eval();
        values.push_back({model.wide[0], model.wide[1], model.wide[2]});
        expected << time << ' ';
        for (int bit = 94; bit >= 0; --bit) expected << ((model.wide[bit / 32] >> (bit % 32)) & 1);
        expected << '\n';
        trace.dump(time);
        context.timeInc(1);
    }
    trace.close();
    if (mode != "vcd") {
        vtr::Reader reader{prefix};
        if (!reader.validate().missing.empty())
            vl_fatal(__FILE__, __LINE__, "", "Invalid VTR output");
        const auto signal = reader.find_signal("TOP.t.wide [94:0]");
        if (!signal) vl_fatal(__FILE__, __LINE__, "", "Missing VTR signal");
        for (unsigned time = 0; time < values.size(); ++time) {
            const auto value = reader.value_at(*signal, begin + time);
            if (value.kind.width != 95 || value.packed.size() != 12)
                vl_fatal(__FILE__, __LINE__, "", "Cannot decode wide VTR value");
            for (unsigned byte = 0; byte < 12; ++byte) {
                const uint8_t expectedByte = values[time][byte / 4] >> ((byte % 4) * 8);
                if (std::to_integer<uint8_t>(value.packed[byte]) != expectedByte)
                    vl_fatal(__FILE__, __LINE__, "", "VTR value differs from evaluated RTL");
            }
        }
        bool enumFound = false, aliasFound = false, packedFound = false;
        for (const auto& node : reader.nodes()) {
            enumFound |= node.kind == vtr::NodeKind::enum_table && node.enum_entries.size() == 5;
            aliasFound |= node.alias;
            packedFound |= node.scope_kind == vtr::ScopeKind::struct_;
        }
        if (!enumFound || !aliasFound || !packedFound)
            vl_fatal(__FILE__, __LINE__, "", "VTR HDL declaration metadata missing");
    }
}

template <class Trace>
void run(const std::string& mode) {
    VerilatedContext context;
    context.threads(2);
    context.traceEverOn(true);
    VM_PREFIX model{&context, "TOP"};
    std::unique_ptr<Trace> tracep;
    if constexpr (std::is_same_v<Trace, VerilatedVtrC>) {
        vtr::WriterOptions options;
        options.mode
            = mode == "vtr-inline" ? vtr::ExecutionMode::inline_ : vtr::ExecutionMode::threaded;
        tracep.reset(new Trace{options});
    } else
        tracep.reset(new Trace);
    Trace& trace = *tracep;
    model.trace(&trace, 99);
    const std::string prefix = VL_STRINGIFY(TEST_OBJ_DIR) "/formats." + mode;
    capture(trace, model, context, prefix, mode);
    capture(trace, model, context, prefix + ".reopen", mode);
    model.final();
    std::puts("*-* All Finished *-*");
}

int main() {
    const char* const modep = std::getenv("VTR_TRACE_MODE");
    if (modep && std::string{modep} != "vcd")
        run<VerilatedVtrC>(modep);
    else
        run<VerilatedVcdC>("vcd");
}
