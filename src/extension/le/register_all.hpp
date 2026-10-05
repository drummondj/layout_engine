#pragma once

namespace le::ext
{
    /// @brief Registers every extension built into this binary (generated
    /// from LE_EXTENSION_DIRS), in dependency order. Idempotent; le_shell
    /// and the le_tcl module each call it at startup.
    void register_all();
}
