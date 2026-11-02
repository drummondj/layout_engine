#pragma once
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <string>
#include <utility>
#include <vector>

// Helpers LEFReader and DEFReader share.
namespace le::reader_messages
{
    /// @brief The calling thread's staging buffer for one read's messages.
    /// The vendored parsers' log callbacks carry no userData, so their text
    /// can't go straight to the reader instance mid-read; it collects here,
    /// and the reader moves it into its own messages_ once the parse
    /// returns. log_warning/log_error append here too, so every diagnostic
    /// from one read stays in one place in chronological order (writing
    /// straight to messages_ would be wiped by that move). Per thread: the
    /// read holds lefdef_mutex(), and the parsers' state is process-wide.
    inline std::vector<std::string> &pending()
    {
        thread_local std::vector<std::string> messages;
        return messages;
    }

    /// @brief Logs a warning to spdlog and stages it. Formats once and logs
    /// the formatted text through "{}": file-controlled content (a layer or
    /// design name) may hold a literal `{` or `}`.
    template <typename... Args>
    void log_warning(fmt::format_string<Args...> fmt_str, Args &&...args)
    {
        std::string msg = fmt::format(fmt_str, std::forward<Args>(args)...);
        spdlog::warn("{}", msg);
        pending().push_back("WARNING: " + msg);
    }

    /// @brief log_warning for an error.
    template <typename... Args>
    void log_error(fmt::format_string<Args...> fmt_str, Args &&...args)
    {
        std::string msg = fmt::format(fmt_str, std::forward<Args>(args)...);
        spdlog::error("{}", msg);
        pending().push_back("ERROR: " + msg);
    }

    /// @brief Stages a parser log line, without its trailing newlines.
    inline void stage_parser_message(const char *msg)
    {
        if (!msg)
            return;
        std::string s(msg);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        if (!s.empty())
            pending().push_back(std::move(s));
    }

    /// @brief A via's MASK as one 3-digit number. The vendored readers
    /// split it into top/cut/bottom digits (lefiMisc.cpp's `topMaskNum =
    /// viaMask / 100`, defiNet's topMaskNum(i) etc.), but the writers only
    /// take the combined number, so ShapeVia/ShapeViaIterate.mask keep it
    /// combined. A 0 digit is unset (per-cut-layer masking is optional).
    inline int combine_via_mask(int top, int cut, int bottom)
    {
        return top * 100 + cut * 10 + bottom;
    }
}
