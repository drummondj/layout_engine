#include "upright_text_canvas.hpp"

// Compiled with -fno-rtti (CMakeLists.txt) - see UprightTextCanvas's own
// destructor declaration comment (upright_text_canvas.hpp) for why: this
// is the class's sole out-of-line virtual, so its vtable is emitted only
// here, in a TU where no `typeinfo for` symbol is ever attempted for
// either UprightTextCanvas or its RTTI-less Skia base.
le::UprightTextCanvas::~UprightTextCanvas() = default;
