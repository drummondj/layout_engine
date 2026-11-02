module fifo #(parameter WIDTH = 4) (input [WIDTH-1:0] d);
endmodule

module fifo_WIDTH8 (input x);
endmodule

module top (input [7:0] a);
  fifo #(.WIDTH(8)) u_w8 (.d(a));
  fifo_WIDTH8 u_real (.x(a[0]));
endmodule
