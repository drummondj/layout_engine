module BUFX1(input logic A, output logic Z);
endmodule

module spike_mixed #(parameter int WIDTH = 2) (
    input  logic clk,
    output logic [WIDTH-1:0] out
);
    genvar i;
    generate
        for (i = 0; i < WIDTH; i = i + 1) begin : gen_buf
            BUFX1 u_buf(.A(clk), .Z(out[i]));
        end
    endgenerate

    always_ff @(posedge clk) begin
        out <= ;
    end
endmodule
