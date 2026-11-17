#pragma once
// Debug builds catch a thread taking a session's lock while it already
// holds it - a second view, a Transaction or a render inside a view - which
// would otherwise deadlock silently (std::shared_mutex isn't recursive, and
// even a second shared lock deadlocks once a writer queues between them).
// Each place that takes LeHandle::mutex_ calls enter() first and leave()
// once it lets go. With NDEBUG both are no-ops.

struct LeHandle;

namespace le::lock_check
{
#ifdef NDEBUG
    inline void enter(const LeHandle *, const char *) {}
    inline void leave(const LeHandle *) {}
#else
    /// @brief Records that this thread is about to lock `handle` for
    /// `what` ("a ReadView", ...); aborts, naming both, if it already holds it.
    void enter(const LeHandle *handle, const char *what);
    /// @brief Forgets the most recent enter() for `handle` on this thread.
    void leave(const LeHandle *handle);
#endif

    /// @brief enter() for this scope's lifetime: declare it just before the
    /// lock it covers.
    class Scope
    {
    public:
        Scope(const LeHandle *handle, const char *what) : handle_(handle) { enter(handle, what); }
        ~Scope() { leave(handle_); }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

    private:
        const LeHandle *handle_;
    };
}
