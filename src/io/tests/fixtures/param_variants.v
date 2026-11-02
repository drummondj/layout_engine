module fifo #(parameter WIDTH = 4, parameter DEPTH = 2) (input [WIDTH-1:0] d, output [WIDTH-1:0] q);
  localparam DOUBLE = WIDTH * 2;
  assign q = d;
endmodule

module tag #(parameter string LABEL = "a") (input x);
endmodule

module top (input [3:0] a, output [3:0] b, input [7:0] c, output [7:0] e, input [15:0] f, output [15:0] g);
  fifo u_default (.d(a), .q(b));
  fifo #(.WIDTH(8)) u_w8 (.d(c), .q(e));
  fifo #(.WIDTH(8'd8)) u_w8_again (.d(c), .q());
  fifo #(.WIDTH(16), .DEPTH(4)) u_w16 (.d(f), .q(g));
  fifo #(.WIDTH(4)) u_explicit_default (.d(a), .q());
  tag #(.LABEL("b")) u_tag (.x(a[0]));
endmodule
