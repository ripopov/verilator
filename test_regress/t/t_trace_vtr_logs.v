// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2003 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module t(input logic clk, input logic [2:0] mode);
   integer fd;
   initial begin
      fd = $fopen("messages.txt", "w");
      $display("first");
      $write("second");
      $fdisplay(fd, "file message");
   end
   always @(posedge clk) begin
      case (mode)
        0: $info("info message");
        1: $warning("warning message");
        2: $error("error message");
        3: $fatal(1, "fatal message");
      endcase
   end
endmodule
