#include "layout_engine_plugin.h"

// Nothing to define here - every le_*() symbol declared in api.hpp is
// provided by linking the backend's compiled api library into this shared
// library (see layout_engine_plugin.h's comment), not by this translation
// unit. This file exists only because the build needs at least one C
// source to compile into the shared library target.
