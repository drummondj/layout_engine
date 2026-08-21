// Relative import to be able to reuse the shared C++ source. See the
// comment in ../layout_engine_plugin.podspec / layout_engine_plugin.c for why
// this forwarder exists - Podspec does not support relative paths outside
// Classes/, so this is the only way to compile ../../src/*.cpp into this
// pod.
#include "../../src/le_tcl_bridge.cpp"
