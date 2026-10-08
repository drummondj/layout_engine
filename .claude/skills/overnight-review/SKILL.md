---
name: overnight-review
description: Overnight review - work through open GitHub issues labelled `overnight`, unattended, one branch and PR per issue
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
runs in: commit to a per-issue branch, push the branch, and open a PR. This
applies only to issues worked through this skill in this run. Never push to
or merge into `main` - the user reviews and merges each PR in the morning.

**Risky changes:** if an issue needed real correction after a wrong first
attempt, or is high blast radius (e.g. a data-correctness change to a file
writer, not UI polish), still commit and push its branch, but open the PR
as a **draft** (`gh pr create --draft`) and add the `needs-signoff` label to
the issue, saying in the PR what needs a careful look. Use this for
genuinely risky cases only, not as a default hedge.

## Steps

1. **List the work:** `gh issue list --label overnight --state open`, then
   `gh issue view N --comments` for each. Group related issues (same
   subsystem) back to back. Start from an up-to-date `main`
   (`git switch main && git pull --ff-only`).

2. **Per issue:**
   - Create its branch from `main`: `git switch -c <N>-<short-slug> main`
     (e.g. `2-port-marker-one-piece`). Each issue gets its own branch so
     each PR can be reviewed and merged independently. If an issue
     genuinely depends on an earlier one from this run, branch from that
     issue's branch instead and say so in the PR.
   - Research the root cause in the actual code and tests. Use an Explore
     or general-purpose agent for broad multi-file searches.
   - Implement the fix or feature, following every standing project
     convention:
     - write tests alongside the code (failing before, passing after, where
       that's meaningful)
     - benchmark before any performance-motivated change
     - build and test in `build` (Debug); `build_release` is only for
       benchmarks
     - fix root causes rather than papering over failures
     - comments describe current behaviour only, per CLAUDE.md's Comments
       section
   - Where the issue is ambiguous, make the most reasonable call yourself.
     There's no one to ask.
   - Run the full suite (`ctest --test-dir build --output-on-failure`)
     before calling an issue done. Never leave a branch broken.
   - Commit with a message that explains the *why*; the commit that
     completes the issue ends with `Fixes #N` (the issue closes when the
     PR is merged).
   - Push the branch (`git push -u origin <branch>`) and open a PR
     (`gh pr create --base main --body-file …`). The PR description is the
     write-up for the morning review:
     - what was wrong or needed
     - the fix, with file:line references
     - any **Judgment call:** what was decided and why, so it's easy to
       override
     - the test coverage added
     - benchmark numbers, if performance-relevant

     Keep it technical and specific, with no filler.
   - Switch back to `main` before starting the next issue.
   - If the issue isn't actually resolved (for example, the bug couldn't be
     reproduced), don't open a PR. Leave the issue open and comment on it
     (`gh issue comment N --body-file …`) with what was tried and any
     working theory, and ask for a better repro.

3. **Don't stop for questions.** Make a reasonable, documented decision
   instead of pausing. The one exception: still avoid destructive or
   irreversible actions outside the normal edit-test-commit loop
   (force-push, deleting unrelated files, touching `main`, etc.).

4. **Follow-ups:** open a new issue for out-of-scope work that came up
   along the way. Label it `bug` or `enhancement`, but not `overnight`;
   the user decides that.

5. **Finish with a closing summary** as the session's final message:
   - what got done (issue numbers, branches and PR links)
   - what's still open, and why
   - which PRs are drafts labelled `needs-signoff`
   - which follow-up issues were opened
