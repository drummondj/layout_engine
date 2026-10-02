module top(input clk, output q);
    BUFX1 u1(.A(clk), .Z(q));
endmodule
