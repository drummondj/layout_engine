#pragma once
#include <le/extension.hpp>

#include <string>

namespace hello
{
    /// @brief This extension's per-session state.
    struct State
    {
        int greeting_libraries_added = 0;
    };

    /// @brief How many libraries the session's database holds (a read).
    int library_count(le::ext::ExtensionContext &ctx);

    /// @brief Adds a greeting library named "hello_<name>", as one undo step (an
    /// undoable edit through the C API). False, changing nothing, if `name`
    /// is empty or the library couldn't be created.
    bool add_greeting_library(le::ext::ExtensionContext &ctx, const std::string &name);
}
