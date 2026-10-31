#pragma once
#include <mutex>

namespace le
{
    /// @brief Serializes every use of the vendored Si2 LEF/DEF parsers and
    /// writers, which keep process-global state (lefData, lefSettings,
    /// defSettings, the lefw*/defw* writer state): read_lef, read_def,
    /// write_lef and write_def each hold it for the whole call. Callers
    /// hold their handle's lock first, never the other way round, so the
    /// two can't deadlock.
    inline std::mutex &lefdef_mutex()
    {
        static std::mutex mutex;
        return mutex;
    }
}
