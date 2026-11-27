// Supply nets: supply1 is POWER, supply0 GROUND, and a port takes its net's use.
module top (input a, output y, inout supply1 VDD, inout supply0 VSS);
  supply0 gnd_int;
  supply1 [1:0] vb;
  wire n1;
  BUFX1 u0 (.A(a), .Y(n1), .VDD(VDD), .VSS(VSS));
  BUFX1 u1 (.A(n1), .Y(y), .VDD(vb[0]), .VSS(gnd_int));
endmodule
