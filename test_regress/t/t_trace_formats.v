// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module t (
  input clk,
  input [6:0] value,
  output logic [94:0] wide
);
  typedef enum logic [2:0] {IDLE, FETCH, ISSUE, EXECUTE, RETIRE} state_t;
  typedef struct packed {
    logic [14:0] address;
    state_t state;
  } sample_t;
  sample_t samples [3:1];
  state_t state;
  wire [94:0] alias_value = wide;

  always_comb begin
    wide = {clk, {13{value}}, 3'b101};
    state = state_t'(value[2:0]);
    for (int i = 1; i <= 3; ++i) begin
      samples[i].address = 15'(value * 29 + 7'(i));
      samples[i].state = state;
    end
  end
endmodule
