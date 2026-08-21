---
name: cpp-review
description: Review pending changes (uncommitted work plus commits ahead of main) in this C++ backend for bugs that should be covered by tests, unnecessary allocations/copies/moves, memory leaks and potential segfaults, and other important issues. Use when the user asks for a code review or to review changes, or invokes /cpp-review. Distinct from the built-in /code-review (which is user-triggered, billed, and cloud-based) - this is a local, free, project-specific review.
user-invocable: true
allowed-tools:
  - Read
  - Grep
  - Bash(git diff:*)
  - Bash(git log:*)
  - Bash(git status:*)
  - Bash(git show:*)
  - Bash(git merge-base:*)
  - ReportFindings
---

# C++ backend code review

Review-only: report findings, don't fix them, unless the user explicitly
asks you to apply fixes afterward.

## 1. Determine scope

```
git status --porcelain
git merge-base main HEAD    # if this fails / HEAD is main, there's no ahead-of-main range
git diff HEAD                              # uncommitted (staged + unstaged)
git diff $(git merge-base main HEAD)..HEAD # commits ahead of main, if any
```

Review the union of both. If there's nothing in either, say so and stop —
don't invent findings.

## 2. Read full context, not just diff hunks

A diff hunk alone is not enough to judge lifetime, ownership, or whether a
branch is tested. For every changed function, read the whole function, its
header declaration, and its existing tests in the sibling `tests/`
directory (e.g. `src/io/lef_reader.cpp` → `src/io/tests/`).

## 3. Checklist

**Bugs that should be covered by tests.** For each new or changed branch,
loop, or edge case, check whether an existing test actually exercises it
(not just the happy path through the same function). A changed condition,
a new early-return, a new default value, a new index/loop bound — each of
these is a candidate for a missing test, and in this codebase that's not
hypothetical: real bugs (a `hasForeignOrigin()` missing its loop index, an
orientation int→enum table with two values swapped, a boost buffer distance
that was double what it should've been) were all sitting in code with 100%
*line* coverage but the wrong *value* asserted, or no assertion on the
specific value that would've caught it. Flag logic with no corresponding
assertion on its actual output, not just "was this line executed."

**Unnecessary allocations, moves, and copies.**
- Any parameter of non-trivial type — `std::string`/`std::vector`/`Shape`/
  any user-defined struct or class, not just stdlib containers — taken by
  value (or by non-const reference when never mutated) where `const&`
  would do and the callee doesn't need its own copy. Check free functions
  and generated code too, not just member functions: a real bug here was
  `cmg`-generated `to_string`/`to_properties`/`operator<<` (one per
  schema class) all taking their whole struct *by value* — invisible for
  small classes, but a 29ms-per-call deep copy for `ObstructionData`,
  whose `shapes` field can hold hundreds of thousands of entries embedded
  directly in the struct (not pool-referenced like `TerminalData`'s
  ports) — called on every property fetch for a selected Obstruction.
  Flag any parameter passed by value into a function that never mutates
  its own copy, and any pass-by-reference parameter missing `const` when
  nothing in the body writes through it.
- Missing `std::move` on a local about to go out of scope (e.g. building a
  `Polygon`/`Shape` then copying it into a container instead of moving it).
- Round-tripping through `Geometry::to_boost_polygon`/`from_boost_polygon`
  (or similar conversions) more times than necessary in one code path.
- Missing `.reserve()` before a loop of known size that `push_back`s.
- Returning a container by value in a way that defeats NRVO (e.g. multiple
  differently-named return paths).
- Capturing by value in a lambda that only reads a large object.

**Memory leaks and potential segfaults.**
- Raw `new`/`malloc`/`fopen`/`lefMalloc`-style resource acquisition without
  a matching, exception-safe release (prefer RAII over manual
  paired-call cleanup).
- Dereferencing the result of `Root::get_*`/`Pool::get` (these return
  nullable `T*` by this project's own convention — see `CLAUDE.md`) without
  a null check.
- Trusting an `Id` (`TechnologyId`, `LayerId`, ...) or a pointer across a
  call that could have invalidated it (the `Pool` is generational — an old
  `Id` silently fails `.valid()`/lookup after erase+reuse, it doesn't
  dangle, but a raw `T*` obtained before a `Pool::create`/`erase` can).
  Reject other memory-unsafety findings after checking `Root::get_*` really
  can invalidate the pointer *within the shown code path* — read `pool.hpp`
  before asserting this.
- Iterator/reference invalidation from mutating a `std::vector` while
  iterating or holding a reference to an element.
- Any raw pointer/reference stored past the lifetime of what it points to
  (e.g. a `const Shape*` into a vector that could reallocate or go out of
  scope).
- Calling a `has*()`-guarded getter on a vendored-parser object (`lefi*`)
  without the guard — the parser reuses scratch structs across callback
  invocations and does not reset fields to a neutral default (see
  `CLAUDE.md`'s Conventions section) — an unguarded getter can silently
  read a value that leaked forward from a previous, unrelated element.

**Anything else important.** Off-by-one errors, incorrect unit conversion
(should go through `microns_to_dbu`, not ad hoc arithmetic), thread-safety
(this project's pipeline is meant to become multi-threaded — flag shared
mutable state with no synchronization plan), and adherence to `CLAUDE.md`'s
stated conventions (pool lookups return nullable pointers rather than
throwing; validate at system/file-parsing boundaries but not on internal
data this code already guarantees — see the `geometry.hpp` vs.
`lef_reader.cpp` dead-code decisions in git history for the reasoning split).

## 4. Verify before reporting

For each candidate, read enough surrounding code to rule out:
- Pre-existing issues outside the changed lines.
- Something that looks wrong but isn't (e.g. a "copy" that's needed because
  the original is mutated afterward).
- Anything a compiler warning or the test suite would already catch — build
  first (`cmake --build build`) and don't re-report existing warnings.
- Nitpicks with no real failure scenario.

Drop anything that doesn't survive this. Set `verdict` (`CONFIRMED` or
`PLAUSIBLE`) on what's left.

## 5. Report

Call `ReportFindings` once, most-severe first, empty array if nothing
survived verification. Use `category` values like `test-coverage`,
`memory-safety`, `performance`, `correctness`. Set `level` to the effort
actually spent (diff size and depth of investigation).
