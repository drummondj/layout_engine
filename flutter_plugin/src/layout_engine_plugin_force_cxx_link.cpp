// Only compiled in when LE_LINK_BACKEND is ON (see CMakeLists.txt). This
// target's real sources are plain C, so CMake would otherwise pick the C
// linker driver for the final link - which doesn't pull in libstdc++/
// libc++ automatically, and the le_*() symbols linked in from the
// backend's static libraries are C++ underneath and need it (operator
// new/delete, __cxa_*, the personality routine). A single C++ source is
// enough to make CMake select the C++ linker driver for the whole target
// (the same fix this plugin's macOS podspec needed, via
// Classes/layout_engine_plugin_force_cxx_link.mm - see that file's comment
// for why a manual -lstdc++/-lc++ flag isn't a reliable substitute there).
