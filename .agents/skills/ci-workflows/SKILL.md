---
name: ci-workflows
description: >
  Trigger CI on a feature branch, watch runs, fetch failed-step logs efficiently,
  and download Playwright artifacts. Covers this repo's GitHub Actions workflows,
  the `gh` CLI patterns that work well for agents, and Codespaces-specific notes
  (preauthenticated `gh`, scope quirks, fork/secret gating).
triggers:
  - run CI
  - trigger CI
  - kick off tests
  - rerun the workflow
  - GitHub Actions
  - workflow run
  - workflow_dispatch
  - check CI status
  - fetch CI logs
  - download artifact
  - Playwright report
  - Playwright trace
  - why did CI fail
  - gh run
  - gh workflow
---

# CI Workflows — Agent Skill

How to trigger, watch, and debug CI for `granny-smith` from a Codespace (or any
shell with `gh` available).

## 1. Workflows in this repo

| File | Name | Auto-trigger | Manual (`workflow_dispatch`) | Purpose |
|------|------|--------------|------------------------------|---------|
| [.github/workflows/tests.yml](../../../.github/workflows/tests.yml) | `CI` | `push` / `pull_request` to `main` | **Yes** | Build, unit, integration, Playwright E2E |
| [.github/workflows/clang-format.yml](../../../.github/workflows/clang-format.yml) | formatting check | `push` / `pull_request` to `main` | **No** | clang-format verification |
| [.github/workflows/build-dev-image.yml](../../../.github/workflows/build-dev-image.yml) | dev image build | `push` to `main` (path-filtered) | **Yes** | Rebuild `ghcr.io/pappadf/granny-smith-dev` |
| [.github/workflows/publish.yml](../../../.github/workflows/publish.yml) | publish | `release: published` | **No** | Publish on GitHub release |

Key fact: **non-main branches do not auto-run CI on push** — only PRs against
`main` trigger it. To verify CI on a feature branch without opening a PR, use
`workflow_dispatch` (only `tests.yml` and `build-dev-image.yml` support this).
`clang-format.yml` can only be exercised via a PR.

## 2. Codespaces context (read this before triggering CI)

### Installing `gh`

Not preinstalled in this repo's dev container or in vanilla Codespaces base
images. Check with `which gh`; if missing:

```bash
sudo apt-get update && sudo apt-get install -y gh
```

### What the auto-injected token can do

`gh` picks up `$GITHUB_TOKEN` automatically — a `ghu_*` GitHub App
installation token issued by the Codespaces app. It works for everything an
agent typically needs **except** triggering workflows:

- ✅ `gh run list / view / view --log-failed / download / watch / rerun`
- ✅ `gh workflow list / view`
- ✅ `gh pr create / view / merge / comment`, `gh issue *`, `git push`
- ❌ `gh workflow run` (workflow_dispatch) — needs `actions: write`, which
  the Codespaces token doesn't carry. Returns `HTTP 403: Resource not
  accessible by integration`.

This is a structural cap on the Codespaces token, not a misconfiguration —
even repo admins hit it. Diagnostic: the 403 response carries the header
`x-accepted-github-permissions: actions=write`, which names exactly what's
missing.

### How to trigger CI from a Codespace

Two reliable paths, both fully in-Codespace:

**a) Open a PR against `main`** (default choice). `tests.yml` auto-runs on
the `pull_request` event — no dispatch permission needed.

```bash
gh pr create --base main --head "$(git branch --show-current)" \
  --title "..." --body "..."
# subsequent pushes to the same branch keep re-triggering CI
```

If a PR is already open, just push (or `git commit --allow-empty -m "ci: retrigger" && git push`) — the existing PR re-runs CI.

**b) `gh auth login` with a user PAT inside the Codespace.** Replaces the
installation token for that shell session and re-enables `gh workflow run`.
Needs a PAT with `repo` + `workflow` scopes (classic) or fine-grained
equivalent (Actions: read & write, Contents: read & write).

```bash
gh auth login           # choose GitHub.com → HTTPS → Paste an authentication token
```

After that succeeds, `gh workflow run tests.yml --ref <branch>` works (§3).
Note: the new auth survives within the Codespace; new shells in the same
Codespace inherit it via `gh`'s config (`~/.config/gh/`).

### Don't bother with these (known dead ends)

- `gh auth refresh -s workflow` — the Codespaces token is not a re-scopeable
  PAT, so this can't elevate it.
- `GH_TOKEN=$GITHUB_CODESPACE_TOKEN gh ...` — that env var is the Codespaces
  management token; the REST API rejects it with `HTTP 401: Bad
  credentials`.
- Adding permissions to `devcontainer.json` `customizations.codespaces` —
  that mechanism is for cross-repo access, not for upgrading the host repo's
  `actions` permission.

`gh auth status` shows the active token type — check it first if a command
fails with 403.

## 3. Triggering CI manually on a feature branch

> **From a Codespace, this command requires `gh` to be authenticated with a
> user PAT** (option **b** in §2) — the auto-injected `GITHUB_TOKEN` lacks
> `actions: write` and returns `HTTP 403: Resource not accessible by
> integration`. If you don't want to set up a PAT, open a PR (option **a**
> in §2) instead. Verify auth with `gh auth status` before running.

```bash
# Current branch
gh workflow run tests.yml --ref "$(git branch --show-current)"

# Specific branch
gh workflow run tests.yml --ref feature/object-model
```

Caveats:
- The workflow file (with `workflow_dispatch:`) must already be **committed and
  pushed on the target branch** — GitHub reads the workflow definition from the
  ref, not from `main`.
- `gh workflow run` returns immediately and does **not** print the new run ID.
  Use the pattern in §4 to grab it.
- A 200/204 from `gh workflow run` only confirms the dispatch was *accepted*.
  Always verify a run actually appeared with `gh run list` (§4) — silent
  no-ops happen when the ref doesn't carry the expected workflow file.

## 4. Finding the run you just triggered

`workflow_dispatch` runs take a few seconds to register. Robust sequence:

```bash
BRANCH="$(git branch --show-current)"
gh workflow run tests.yml --ref "$BRANCH"

# Wait for the run to appear, then capture its ID
sleep 3
RUN_ID=$(gh run list \
  --workflow=tests.yml \
  --branch "$BRANCH" \
  --event workflow_dispatch \
  --limit 1 \
  --json databaseId -q '.[0].databaseId')
echo "Run: $RUN_ID"
```

Alternative — list recent runs in a readable form:

```bash
gh run list --workflow=tests.yml --branch "$BRANCH" --limit 5
```

## 5. Watching / polling status

**Expect a full `tests.yml` run to take ~20–25 minutes** end-to-end (build +
unit + integration + Playwright E2E in the dev container). Plan around that:
do not poll aggressively, do not block in the foreground, and do not declare
"CI is stuck" before ~30 minutes.

Three patterns, in order of preference for an agent:

### a) Background + scheduled wakeup (best for long waits)

Trigger the run, then use the harness's scheduled wakeup so the agent isn't
sitting in a hot loop. Example cadence: first check at ~15 min, then every
3–5 min until conclusion. The 20–25 min total means polling more than once
every couple of minutes is wasted effort.

### b) Blocking wait with `gh run watch`

```bash
gh run watch "$RUN_ID" --exit-status     # blocks; non-zero exit on failure
gh run watch "$RUN_ID" --interval 30     # default poll interval is 3s (too chatty)
```

Use only when you genuinely need to block on the result and have nothing else
to do. Pair with `timeout` so a stuck runner doesn't hang forever:

```bash
timeout 35m gh run watch "$RUN_ID" --exit-status --interval 30
```

### c) Manual poll (one-shot status check)

```bash
gh run view "$RUN_ID"                                # human-readable, job/step level
gh run view "$RUN_ID" --json status,conclusion,jobs  # structured

# Compact one-liner for scripts / agent loops:
gh run view "$RUN_ID" --json status,conclusion -q '.status + " " + (.conclusion // "")'
# "queued "             → not started
# "in_progress "        → still running
# "completed success"   → green
# "completed failure"   → failed
# "completed cancelled" → cancelled
```

Status values to know: `queued`, `in_progress`, `completed`. Conclusion is
empty until status is `completed`, then one of `success`, `failure`,
`cancelled`, `skipped`, `timed_out`, `action_required`, `neutral`.

## 6. Reading logs (cheaply)

A successful `tests.yml` run produces **~3,000 lines** of step output (build,
emcc verbose output, unit test runner, integration tests, Playwright reporter).
Failures push it higher. Pulling the full log into agent context wastes a large
chunk of the window and buries the actual signal under build noise.

`gh run view --log` dumps **every** step from **every** job. Almost always
wrong for agents. Prefer:

```bash
# Only failed steps — usually 50-300 lines, fits comfortably in context
gh run view "$RUN_ID" --log-failed

# Logs from a specific job (find job IDs with: gh run view "$RUN_ID")
gh run view --job=<job_id> --log
```

If `--log-failed` is still huge, narrow further:

```bash
gh run view "$RUN_ID" --log-failed | grep -E "FAIL|Error|✗|error:" -B 2 -A 10
```

## 7. Artifacts produced by `tests.yml`

The CI job uploads two artifacts on Playwright runs (only when proprietary test
data is available — see §9):

| Artifact | When | Contents |
|----------|------|----------|
| `playwright-report` | Always (if test data available) | HTML report (`playwright-report/`) |
| `playwright-traces-videos` | On failure only | Per-test `.zip` traces, `.webm`/`.mp4` videos, screenshots from `tests/e2e/test-results/` |

There is **no JUnit/JSON structured artifact** at present — failed tests must be
read either from `--log-failed` or by opening a Playwright trace.

### Downloading

```bash
# Everything from a run, into ./artifacts/
gh run download "$RUN_ID" --dir artifacts/

# Just one artifact
gh run download "$RUN_ID" --name playwright-report --dir artifacts/

# Latest run on a branch (no ID needed)
gh run download --branch feature/object-model --name playwright-report
```

### Inspecting a Playwright trace

Traces are zips containing a JSON action log + screenshots. For an agent, the
useful payload is usually `trace.zip → test-results/<name>/` plus the
console/network logs inside. Quickest read:

```bash
unzip -p artifacts/playwright-traces-videos/<test>/trace.zip trace.trace | head -200
```

The HTML report is for humans — agents should grep the raw test output via
`--log-failed` first.

## 8. Re-running

```bash
gh run rerun "$RUN_ID"                   # full rerun
gh run rerun "$RUN_ID" --failed          # only failed jobs
gh run rerun "$RUN_ID" --job=<job_id>    # one specific job
```

`--failed` is the right default for flaky-test triage.

## 9. Test data gating (important)

`tests.yml` checks for the `GS_TEST_DATA_TOKEN` secret in the **first step**
(`Fetch test data`) and sets `has_test_data=true|false`. Downstream steps gate
on this output:

- **`has_test_data=true`** (internal pushes & PRs from branches with secret
  access): unit + integration + Playwright E2E all run; both artifacts upload.
- **`has_test_data=false`** (external fork PRs, or any context where secrets
  are unavailable): only **CPU unit tests** run; Playwright steps are skipped;
  **no Playwright artifacts are produced**.

If you trigger a run and the artifact list is empty, this is the most likely
reason — check the run's `Fetch test data` step output or the `Summary for
builds without test data` step. It's not a CI bug.

## 10. Common one-liners

```bash
# "Did the latest CI on my branch pass?"
gh run list --workflow=tests.yml --branch "$(git branch --show-current)" --limit 1

# "Show me why the latest run failed, briefly"
RUN=$(gh run list --workflow=tests.yml --branch "$(git branch --show-current)" \
       --limit 1 --json databaseId -q '.[0].databaseId')
gh run view "$RUN" --log-failed

# "Trigger CI and wait for the result" (full run is ~20-25 min)
gh workflow run tests.yml --ref "$(git branch --show-current)"
sleep 4
RUN=$(gh run list --workflow=tests.yml --branch "$(git branch --show-current)" \
       --event workflow_dispatch --limit 1 --json databaseId -q '.[0].databaseId')
timeout 35m gh run watch "$RUN" --exit-status --interval 30

# "Grab Playwright traces from the most recent failed run on this branch"
gh run download --branch "$(git branch --show-current)" --name playwright-traces-videos
```

## 11. Pitfalls

- `gh workflow run` succeeds even when it queues nothing useful (e.g. ref
  doesn't have the workflow file with `workflow_dispatch:` yet). Always verify
  by listing runs after triggering.
- `gh run watch` without an ID is interactive — agents must capture the ID
  first.
- Pre-existing `in_progress` runs on the same branch can fool a "latest run"
  query into picking the wrong one. Filter by `--event workflow_dispatch` (or
  by the timestamp range) when correctness matters.
- `--log` is enormous (full job logs). Default to `--log-failed` for failure
  diagnosis; reach for `--log` only when investigating a step that succeeded
  but produced unexpected output.
- Workflow files only take effect from the ref they exist on. Editing
  `tests.yml` on `main` does not change CI behavior on a long-lived feature
  branch until you merge / rebase.
- The Codespaces `GITHUB_TOKEN` cannot push commits that modify
  `.github/workflows/*.yml` without the `workflow` scope; use
  `gh auth refresh -s workflow` if you need to commit workflow changes.

## 12. Cross-references

- Workflow definitions: [.github/workflows/](../../../.github/workflows/)
- Test data setup: [docs/TEST_DATA.md](../../../docs/TEST_DATA.md)
- E2E test config: [tests/e2e/playwright.config.ts](../../../tests/e2e/playwright.config.ts)
- Project conventions: [AGENTS.md](../../../AGENTS.md)
