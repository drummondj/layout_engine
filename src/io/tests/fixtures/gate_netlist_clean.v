module INV(input A, output Y);
endmodule

module AND2(input A, input B, output Y);
endmodule

module top #(parameter WIDTH = 2) (
    input clk,
    input [WIDTH-1:0] in,
    output [WIDTH-1:0] out
);
    wire [WIDTH-1:0] mid;

    genvar i;
    generate
        for (i = 0; i < WIDTH; i = i + 1) begin : gen_inv
            INV u_inv(.A(in[i]), .Y(mid[i]));
        end
    endgenerate

    AND2 u_and0(.A(mid[0]), .B(clk), .Y(out[0]));
    AND2 u_and1(mid[1], clk, out[1]);
endmodule
