`include "defs.svh"
package pkg;
  typedef enum logic [1:0] {IDLE, RUN} state_t;
  typedef struct packed { logic valid; logic [7:0] data; } pkt_t;
  function automatic logic [7:0] sat(input logic [7:0] v);
    return v > `LIMIT ? `LIMIT : v;
  endfunction
endpackage

interface bus_if(input logic clk);
  logic valid;
  logic [7:0] data;
  modport master(output valid, output data, input clk);
endinterface

module lane #(parameter int W = 8, parameter bit FAST = 0)(
  input logic clk, input logic rst_n, input logic [W-1:0] d, output logic [W-1:0] q);
  import pkg::*;
  state_t state;
  if (FAST) begin : g_fast
    always_ff @(posedge clk) q <= d;
  end else begin : g_slow
    always_ff @(posedge clk or negedge rst_n)
      if (!rst_n) q <= '0; else q <= sat(d);
  end
endmodule

module top(input logic clk, input logic rst_n, input logic [7:0] in, output logic [7:0] out);
  pkg::pkt_t pkt;  // comment here
  bus_if bus(.clk(clk));
  /* block
     comment */
  lane #(.W(8), .FAST(1)) u_fast(.clk(clk), .rst_n(rst_n), .d(in), .q(pkt.data));
  lane #(.FAST(0)) u_slow(.clk, .rst_n, .d(pkt.data), .q(out));
  assign bus.valid = pkt.data != 0;
  always_comb pkt.valid = u_fast.q[0] & (pkt.data == `LIMIT);
endmodule
