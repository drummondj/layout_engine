#pragma once
#include <le/extension.hpp>

#include <string>
#include <vector>

namespace hello
{
    /// @brief This extension's per-session state.
    struct State
    {
        int libraries_added = 0;
    };

    /// @brief How many libraries the session's database holds (a read).
    int library_count(le::ext::ExtensionContext &ctx);

    /// @brief Adds a library named "hello_<name>", as one undo step (an
    /// undoable edit through the C API). False, changing nothing, if `name`
    /// is empty or the library couldn't be created.
    bool add_library(le::ext::ExtensionContext &ctx, const std::string &name);

    /// @brief The texts of the notes (HelloNote, this extension's own
    /// database class) on the library named `library`, in order; empty if
    /// there's no such library.
    std::vector<std::string> notes_on(le::ext::ExtensionContext &ctx, const std::string &library);
}
