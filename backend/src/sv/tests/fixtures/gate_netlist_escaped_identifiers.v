// A synthesizer-flattened unpacked-array element gets a Verilog escaped
// identifier (`\name`, terminated by whitespace) since a plain identifier
// can't contain '['/']' - the real-world pattern this fixture reproduces
// verbatim (confirmed against real synthesized DEF/Verilog test data):
// a scalar escaped net with no further indexing (`\rkey[1] `), and a
// vector escaped net (32 bits wide) with a real bit-select on top
// (`\block_reg[0] [31]`). Both BUFX1/BUF_X2 here are deliberately never
// defined, so their own connections route through the undefined-leaf-cell
// path (classify_simple_connection_text), matching the real fixture.
module top(input clk);
    wire \rkey[1] ;
    wire [31:0] \block_reg[0] ;

    BUFX1 u_scalar(.A(clk), .Z(\rkey[1] ));
    BUFX1 u_vector_bit(.A(clk), .Z(\block_reg[0] [31]));
endmodule
