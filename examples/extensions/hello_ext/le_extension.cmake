le_add_extension(hello_ext
    CORE_SOURCES  src/hello.cpp
    CORE_INCLUDE  include
    TESTS         tests/hello_test.cpp
    TCL_SWIG      tcl/hello_ext.i
    TCL_SOURCES   tcl/hello_tcl.cpp
    TCL_INIT
)
