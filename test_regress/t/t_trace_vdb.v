// DESCRIPTION: Verilator: Native RTL VDB export.
//
// This file ONLY is placed into the Public Domain, for any use,
// without warranty, 2026.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0
module stage #(parameter W=7) (
  input logic clk, rst_n, en,
  input logic [W-1:0] d,
  output logic [W-1:0] q, spare
);
  assign spare = '0;
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) q <= '0;
    else if (en) q <= d;
  end
endmodule
module t (
  input logic clk, rst_n, en, sel,
  input logic [14:0] a, b,
  output logic [14:0] q, unsupported_q
);
  logic [14:0] tmp, mux;
  always_comb begin
    tmp = a + 15'd1;
    mux = tmp;
    if (sel) mux = b;
  end
  stage #(.W(15)) u(.clk(clk), .rst_n(rst_n), .en(en), .d(mux), .q(q), .spare());
  for (genvar i = 0; i < 2; ++i) begin : lanes
    wire [6:0] value;
    stage v(.clk(clk), .rst_n(rst_n), .en(en), .d(7'(a)), .q(value), .spare());
  end
  always_comb case (sel)
    0: unsupported_q = a;
    default: unsupported_q = b;
  endcase
endmodule
