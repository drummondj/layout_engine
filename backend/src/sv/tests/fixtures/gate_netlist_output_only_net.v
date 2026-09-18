module BUF1(input A, output Y);
endmodule

module WIDEOUT(output [3:0] D);
endmodule

module top(input in);
    wire dangling;
    wire [3:0] wide_dangling;
    BUF1 u_buf(.A(in), .Y(dangling));
    WIDEOUT u_wide(.D(wide_dangling));
endmodule
