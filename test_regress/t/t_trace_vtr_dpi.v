// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

`include "vtr_trace.sv"

module t(input logic clk);
  timeunit 1ps;
  timeprecision 1ps;
  import vtr_trace::*;

  vtr_clock_t schedule;
  vtr_tracker_t pipeline;
  vtr_tracker_t requests;
  vtr_keyspace_t fetched;
  vtr_keyspace_t loads;
  int unsigned sequence_number = 0;

  initial begin
    schedule = vtr_clock("/TOP.t", "schedule");
    pipeline = vtr_pipeline("/TOP.t", "instructions", "schedule");
    requests = vtr_tracker("/TOP.t", "requests", "LSU", "");
    fetched = vtr_keyspace(pipeline, "fetched");
    loads = vtr_keyspace(requests, "loads");
  end

  always @(posedge clk) begin
    if (sequence_number == 0) vtr_clock_run(schedule, 2, VTR_PS);
    if (sequence_number >= 2) begin
      if ((sequence_number & 1) == 0)
        vtr_item_close(loads, vtr_key_t'(sequence_number) - 64'd2, VTR_TX_OK);
      vtr_item_close(fetched, vtr_key_t'(sequence_number) - 64'd2, VTR_TX_OK);
    end
    if (sequence_number > 0) begin
      vtr_item_stage(fetched, vtr_key_t'(sequence_number) - 64'd1, "EX");
      vtr_item_event(fetched, vtr_key_t'(sequence_number) - 64'd1, "issue");
    end
    vtr_item_open(fetched, vtr_key_t'(sequence_number), "IF");
    vtr_item_attr_u64(fetched, vtr_key_t'(sequence_number), "pc", 64'h80000000 + 64'(sequence_number) * 4);
    vtr_item_label(fetched, vtr_key_t'(sequence_number), $sformatf("instruction%0d", sequence_number));
    if ((sequence_number & 1) == 0) begin
      vtr_item_open_child(loads, vtr_key_t'(sequence_number), "Q", fetched, vtr_key_t'(sequence_number));
      vtr_item_relate("load", fetched, vtr_key_t'(sequence_number), loads, vtr_key_t'(sequence_number));
    end
    sequence_number++;
  end

  final begin
    int ignored;
    ignored = vtr_tracker_abort(pipeline);
    ignored = vtr_tracker_abort(requests);
    vtr_clock_stop(schedule);
    $display("DPI complete %0d", sequence_number);
  end
endmodule
