# Overnight review: work through BUGS_AND_ENHANCEMENTS.md unattended

Triggered by a request like "work through BUGS_AND_ENHANCEMENTS.md
overnight" / "for tonight, work through remaining items" - an unattended,
multi-hour pass through the project's own backlog file, with no user
available to answer questions as they come up. `args` (optional) can
narrow scope to specific items (e.g. "just the B-numbered bugs") - default
is every open item in the file.

## Standing exception this skill grants

The project's own general rule is "don't commit/push without the user
reviewing first, even after an earlier commit-and-push in the same
session" (see memory `feedback_commit_review`). Invoking this skill *is*
that review/approval, once, for the whole session it runs in - commit
**and push** per finished item is the default here, matching every prior
overnight pass (see `git log`: "Fix B3...", "Fix B5...", one clean commit
per item, all pushed). This exception is scoped to items completed via
this skill in this run only; it doesn't change how normal daytime
conversation turns work.

**Exception to the exception:** if an item took significant back-and-forth
to get right, needed real correction after an initial wrong attempt, or
is unusually large/high-blast-radius (something a mistake in would be
costly - e.g. a data-correctness change to a file writer, not a UI
polish), leave that one item's change **uncommitted** and flag it clearly
in `OVERNIGHT_REVIEW.md` asking for sign-off before it's committed. This
happened for roughly 1 in 8-10 items historically - the default is still
commit+push, this is a deliberate carve-out for genuinely risky/uncertain
cases, not a hedge to reach for by default.

## Steps

1. **Fresh review doc.** Delete the previous `OVERNIGHT_REVIEW.md` (the
   user says so explicitly when invoking this, e.g. "you may delete last
   night's" - if a session ever invokes this skill without that
   permission being clear, ask first rather than assume) and start a new
   one at the repo root with a header naming tonight's date range and
   this same "research -> implement -> test -> commit+push per item"
   pattern.

2. **Read `BUGS_AND_ENHANCEMENTS.md`** (repo root) in full. Work every
   unchecked `[ ]` item under BUGS and ENHANCEMENTS, in whatever order
   groups related items together (e.g. two items about the same writer/
   subsystem back to back) - skip anything explicitly marked "SKIP FOR
   NOW" or similar in its own line. A QUESTIONS section (if present) isn't
   a task list - answer/investigate opportunistically if time allows and
   record the finding, but don't block on it or treat it as required.

3. **Per item, in order:**
   - Research root cause directly (read the actual code/tests) - use an
     Explore or general-purpose agent for a broad, multi-file codebase
     search rather than grepping by hand turn after turn, same as any
     other task.
   - Implement the fix/feature. Follow every other standing project
     convention exactly as in a normal session - tests written alongside
     the code (verified to fail before the fix and pass after, where that
     distinction is meaningful), benchmark before any performance-
     motivated change, rebuild **both** `build` and `build_release` after
     a backend source change, investigate root causes rather than
     papering over failures, real compiler/runtime verification over
     stale IDE diagnostics.
   - Where the item's own wording is ambiguous or underspecified: make
     the most reasonable call yourself (there's no one to ask) and record
     it explicitly in `OVERNIGHT_REVIEW.md` under that item as a
     "**Judgment call:**" note - what was decided and why, so it's easy
     for the user to spot and override in the morning.
   - Run the full test suite (`ctest --test-dir build --output-on-failure`)
     before considering an item done - never leave the tree in a broken
     state between items, since there's no one to catch it mid-run.
   - Check the item's own checkbox `[x]` in `BUGS_AND_ENHANCEMENTS.md`
     once genuinely verified complete (not just attempted) - leave it
     `[ ]`, with a clear note why, for anything not actually resolved
     (e.g. couldn't reproduce a reported bug - see the "couldn't
     reproduce" shape in a past `OVERNIGHT_REVIEW.md` entry for the tone
     to match: state what was tried, a working theory if any, and ask for
     a better repro rather than silently checking it off).
   - Commit (message describing the *why*, matching this repo's own
     commit-message conventions) and push, unless the "exception to the
     exception" above applies.
   - Append a section to `OVERNIGHT_REVIEW.md` for the item: what was
     wrong/needed, the fix, judgment calls, test coverage added, and
     benchmark numbers if performance-relevant. Match the tone/depth of
     entries in a prior night's file (`git log -- OVERNIGHT_REVIEW.md` to
     find one, or the current file's own earlier entries once a few
     items in) - technical, specific, file:line references, no filler.

4. **Don't stop for questions.** This is unattended overnight work - bias
   fully toward making a reasonable, well-documented decision over
   pausing. The one exception is the same one that always applies:
   genuinely destructive/irreversible actions outside the normal edit-
   test-commit loop (force-push, deleting unrelated user files, etc.) -
   still avoid those even overnight.

5. **Finish with a short closing summary** at the bottom of
   `OVERNIGHT_REVIEW.md` - what got done, what's left open and why,
   what needs the user's sign-off (linking back to those items' own
   sections), and any follow-on items worth adding to
   `BUGS_AND_ENHANCEMENTS.md` for a future night that came up along the
   way but were out of scope for the item that surfaced them.
