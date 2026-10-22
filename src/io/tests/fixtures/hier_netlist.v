// Two levels of hierarchy: `mid` instantiated twice, a top-level instance
// sharing a name with one inside `mid`, and a nested name containing "/".
module leaf(input a, output y);
  assign y = a;
endmodule

module mid(input a, output y);
  wire n_mid;
  leaf u_leaf(.a(a), .y(n_mid));
  leaf \u/slash (.a(n_mid), .y(y));
endmodule

module top(input a, output y);
  wire n1, n2;
  mid u_mid(.a(a), .y(n1));
  mid u_mid2(.a(n1), .y(n2));
  leaf u_leaf(.a(n2), .y(y));
endmodule
