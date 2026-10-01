---
name: overnight-review
description: Overnight review - work through open GitHub issues labelled `overnight`, unattended
---

# Overnight review: work through `overnight` issues unattended

Triggered by a request like "work through the overnight issues tonight".
This is an unattended, multi-hour pass through the project's backlog, with
no user available to answer questions. `args` (optional) narrows the scope,
for example to specific issue numbers or "just bugs". By default the pass
covers every open issue labelled `overnight`.

## Standing exception this skill grants

Normally nothing is committed or pushed without the user reviewing it
first. Invoking this skill *is* that approval, once, for the session it
runs in: commit **and push** each finished issue as one clean commit. This
applies only to issues completed through this skill in this run.

**Exception to the exception:** if an issue needed real correction after a
wrong first attempt, or is high blast radius (e.g. a data-correctness
change to a file writer, not UI polish), leave that change **uncommitted**.
Add the `needs-signoff` label to the issue and comment explaining what's
pending. Use this for genuinely risky cases only (historically about 1 in
8-10 issues), not as a default hedge.

## Steps

1. **List the work:** `gh issue list --label overnight --state open`, then
   `gh issue view N --comments` for each. Group related issues (same
   subsystem) back to back.

2. **Per issue:**
   - Research the root cause in the actual code and tests. Use an Explore
     or general-purpose agent for broad multi-file searches.
   - Implement the fix or feature, following every standing project
     convention:
     - write tests alongside the code (failing before, passing after, where
       that's meaningful)
     - benchmark before any performance-motivated change
     - rebuild **both** `build` and `build_release` after a C++ change
     - fix root causes rather than papering over failures
     - comments describe current behaviour only, per CLAUDE.md's Comments
       section
   - Where the issue is ambiguous, make the most reasonable call yourself.
     There's no one to ask.
   - Run the full suite (`ctest --test-dir build --output-on-failure`)
     before calling an issue done. Never leave the tree broken between
     issues.
   - Commit with a message that explains the *why* and ends with
     `Fixes #N`, then push. Pushing closes the issue. Skip this if the
     "exception to the exception" applies.
   - Comment on the issue (`gh issue comment N --body-file …`) with:
     - what was wrong or needed
     - the fix, with file:line references
     - any **Judgment call:** what was decided and why, so it's easy to
       override
     - the test coverage added
     - benchmark numbers, if performance-relevant

     Keep it technical and specific, with no filler.
   - If the issue isn't actually resolved (for example, the bug couldn't be
     reproduced), leave it open. Comment with what was tried and any working
     theory, and ask for a better repro.

3. **Don't stop for questions.** Make a reasonable, documented decision
   instead of pausing. The one exception: still avoid destructive or
   irreversible actions outside the normal edit-test-commit loop
   (force-push, deleting unrelated files, etc.).

4. **Follow-ups:** open a new issue for out-of-scope work that came up
   along the way. Label it `bug` or `enhancement`, but not `overnight`;
   the user decides that.

5. **Finish with a closing summary** as the session's final message:
   - what got done (issue numbers and commits)
   - what's still open, and why
   - what's labelled `needs-signoff`
   - which follow-up issues were opened
