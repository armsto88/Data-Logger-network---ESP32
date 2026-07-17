# Legacy Positron Agent Workflow Archive

**Original material:** 2026-04-12 to 2026-05-24

**Consolidated:** 2026-07-17

**Status:** historical reference; not the active FieldMesh agent workflow

This document consolidates the former `docs/legacy-agent-workflow/` role files and native-app orchestrator work package. The repeated Positron frontmatter, duplicated safety text, and obsolete model/tool declarations were removed. The workflow logic, approval gates, role boundaries, brief formats, task breakdown, and quality gates are retained here.

The active repository instructions are now:

- `.github/copilot-instructions.md`
- `.github/agents/`
- `.github/skills/`

Do not copy the historical model names or Positron tool declarations from this archive into current automation without validating the current editor/tool contract.

## 1. Historical system purpose

The legacy system coordinated R and Python work through specialist agents. Its key design idea was separation of planning, implementation, review, documentation, and Git operations, with explicit approval before implementation and before external Git actions.

### Historical roles

| Role | Primary responsibility | Write access in the historical design |
|---|---|---|
| Orchestrator | Decompose work, delegate, reconcile results, enforce gates | No application-code edits |
| Planner | Research code and APIs, identify edge cases, produce an ordered plan | Read only |
| Designer | Specify architecture, APIs, Shiny UI/reactivity, and package structure | Read only |
| Coder | Implement an approved multi-file plan and run tests | Code and tests |
| FastCoder | Handle an obvious one-file change, escalating if scope grew | One file |
| Reviewer | Compare implementation with plan, conventions, and runtime evidence | Read only |
| Documentation | Maintain README, roxygen2, docstrings, vignettes, NEWS, and changelogs | Documentation only |
| GitOps | Create branches, commits, pushes, PRs, tags, or releases after approval | Git operations only |
| AI Builder | Maintain the global Positron agent definitions and reusable skills | Agent configuration |

## 2. Historical default workflow

```text
User request
  -> Orchestrator
      -> Planner research and plan
      -> Designer specification when architecture/UI was involved
      -> Approval gate 1: user approves implementation
      -> Coder or FastCoder implements and tests
          -> Reviewer loop (maximum three cycles; two for FastCoder)
      -> Approval gate 2: user approves Git actions
      -> Documentation update when needed
      -> GitOps performs only the approved branch/commit/push/PR action
```

The receiving agent was assumed to have only its handoff brief, not the preceding conversation. Briefs therefore had to be self-contained, concrete, and free of references such as “above” or “the conversation.”

### Scope rule

- One obvious file: FastCoder.
- Multiple files, new features, architectural decisions, complex refactors, or UI/UX decisions: Planner/Designer plus Coder.
- If a FastCoder task expanded beyond one file, it returned to the full workflow.

### Approval and Git safety

- Implementation required explicit approval after the plan.
- Git writes required a second explicit approval.
- No force push.
- No branch deletion, tag, release, or direct commit to `main` without explicit approval.
- Read-only inspection such as `git status`, `git diff`, and `git log` was permitted to coding/review roles.

## 3. Historical handoff contracts

### 3.1 Implementation brief

```markdown
# Implementation Brief

## Objective
[One-sentence goal]

## Context
[Essential current state and reason]

## Plan
| Step | File | Action |
|---|---|---|
| 1 | `path/to/file` | Exact requested change |

## Constraints
- Behaviour, compatibility, and scope limits

## Acceptance criteria
- [ ] Observable completion condition

## Test strategy
- Commands and focused runtime checks
- Expected data shapes, API parameters, or UI behaviour

## Relevant skills
- Applicable reusable instructions
```

### 3.2 Documentation brief

```markdown
# Documentation Brief

## Objective
[What must be documented]

## Changes made
| File | Change |
|---|---|
| `path/to/file` | Implemented behaviour |

## Documentation tasks
- [ ] README/API/changelog work

## Style notes
- Project conventions and audience
```

### 3.3 GitOps brief

```markdown
# GitOps Brief

## Action
[commit | commit and push | commit, push, and PR]

## Branch
- Base and target

## Commit message
[type and description]

## PR details
- Title, body, base, related issue

## Files changed
- Explicit list

## Verification
- Tests and review verdict
```

## 4. Role-specific rules retained from the archive

### 4.1 Planner

The Planner searched related code, read relevant files, checked existing patterns, reviewed repository metadata, and verified external API assumptions before producing a compact plan.

Required plan sections were:

- Summary.
- Files to modify.
- Ordered implementation steps.
- Edge cases.
- Unit, integration, and manual testing strategy.
- Open questions.
- Risks and mitigations.

The Planner did not implement or dump raw research into the final plan.

### 4.2 Designer

The Designer owned architecture and specifications, not implementation. Each decision described:

- Component name, type, and purpose.
- Inputs, outputs, side effects, and errors.
- Files, dependencies, patterns, and complexity.
- UI accessibility considerations where relevant.

Historical specialisms included R package APIs, Shiny/bslib layouts and reactive flow, Python modules/classes, and integration boundaries.

### 4.3 Coder and FastCoder

The Coder followed the approved plan, used existing conventions, ran tests, captured results, and invoked the Reviewer. After a failed review, it fixed critical/warning findings, reran tests, and requested review again, stopping after three unsuccessful cycles.

The FastCoder used the same pattern for a clearly localised one-file task, with at most two review cycles before escalation.

Historical language standards included:

- R: roxygen2, `|>` rather than `%>%`, no `library()` in package code, flat `R/`, and tests in `tests/testthat/`.
- Python: public type annotations, Google-style docstrings, and the repository `pyproject.toml` conventions.
- R test commands: `devtools::test()`, focused `devtools::test(filter=...)`, and `devtools::check()`.
- Python test commands: `pytest` and focused test paths.

The archive required runtime evidence for functions that fetched/transformed data, Shiny reactive paths, external API parameter names, and statistical/model output types. The evidence typically included `str()`, `names()`, `nrow()`, or `head()` rather than relying only on static inspection.

### 4.4 Reviewer

The Reviewer was a read-only gate. It checked:

- Plan and acceptance-criteria compliance.
- Naming, configuration, error handling, and edge cases.
- R/Python conventions.
- Test existence and adequacy.
- Runtime/console evidence and API parameter validity.

Findings were grouped as:

| Severity | Meaning | Required response |
|---|---|---|
| Critical | Broken functionality, failed tests, or core-pattern violation | Must fix |
| Warning | Missing test, code smell, or meaningful deviation | Should fix |
| Note | Style or future optimisation | Optional |

The verdict was explicit `PASS` or `FAIL (cycle N/3)`. After three failed cycles, unresolved findings were escalated to the user.

### 4.5 Documentation

The Documentation role changed documentation rather than application logic and matched the repository’s current style. Its historical scope included:

- R roxygen2 and runnable examples.
- Python Google-style docstrings.
- README and contributor documentation.
- Quarto/R Markdown vignettes.
- NEWS/CHANGELOG sections for features, fixes, and breaking changes.
- Accessible alternative text.

### 4.6 GitOps

The GitOps role operated only from an approved GitOps brief. It verified branch, status, and remote before action; showed the intended commands; avoided force push; and reported branch/PR identifiers after completion.

Historical naming used `feat/`, `fix/`, `docs/`, and `refactor/` branches and conventional commit prefixes such as `feat:`, `fix:`, `docs:`, `refactor:`, `test:`, and `chore:`.

### 4.7 AI Builder

The AI Builder maintained global Positron-era prompts under `~/.config/Positron/User/prompts/*.agent.md` and skills under `~/.agents/skills/`. It distinguished an agent (role and workflow owner) from a reusable skill (focused domain procedure), validated agent frontmatter, and preserved the two approval gates.

The historical agent frontmatter fields included `name`, `description`, `argument-hint`, `tools`, `agents`, `model`, `user-invocable`, `disable-model-invocation`, `target`, and `handoffs`. These fields are reference only and may not match current tooling.

## 5. Native app orchestrator work package

**Original work package date:** 2026-04-12

**Historical primary reference:** `docs/NATIVE_APP_FULL_CONCEPT_AND_SCHEMAS.md`

The work package converted the native-app specification into dependency-aware research, build, and review tasks. Its firmware paths predated the current repository layout, so paths must be revalidated before reuse.

### 5.1 Workstreams

| ID | Workstream |
|---|---|
| WS-01 | Product and requirements consolidation |
| WS-02 | Protocol and integration research |
| WS-03 | App architecture scaffold |
| WS-04 | Command pipeline and transport |
| WS-05 | Fleet UX screens |
| WS-06 | Exports and diagnostics |
| WS-07 | Reliability and security hardening |
| WS-08 | QA, review, and release readiness |

WS-01 and WS-02 began in parallel. WS-03 depended on both; WS-04 depended on WS-03; WS-05 depended on WS-03/04; WS-06 depended on WS-04 and part of WS-05; WS-07 depended on WS-04/05/06; final WS-08 sign-off depended on all prior work.

### 5.2 Research tasks

| ID | Goal | Key output/acceptance |
|---|---|---|
| R-01 | Audit protocol parity for sensor identity and quality fields | Gap report; every mismatch classified as fix-now, acceptable, or blocked |
| R-02 | Research iOS/Android BLE behaviour | Platform risk report and builder constraints |
| R-03 | Research export/save/share behaviour | Platform-specific export recommendations |
| R-04 | Define intermittent-connectivity reliability tests | Reliability test matrix |

### 5.3 Build tasks

| ID | Goal | Dependencies and acceptance |
|---|---|---|
| B-01 | App scaffold and module layout | After R-01/R-02; builds on targets and matches module boundaries |
| B-02 | Typed envelope/message models | After B-01/R-01; parser/serialisation tests and complete sensor identity |
| B-03 | Command state machine and correlation tracking | After B-02; queued -> sent -> acknowledged -> completed/failed tests |
| B-04 | BLE transport adapter | After B-03/R-02; framing/reassembly integration tests |
| B-05 | Local persistence | After B-02/B-03; migrations and offline recovery tests |
| B-06 | Dashboard and node UI | After B-04/B-05; sensor ID/type/label rendered correctly |
| B-07 | Discover/pair/deploy/revert/unpair/schedule/time actions | After B-04/B-06; confirmations and feedback tested |
| B-08 | Export and diagnostics UI | After B-04/B-05/R-03; export flow tested |
| B-09 | Security/reliability hardening | After B-07/B-08/R-04; retry, stale-command, and negative-path tests |
| B-10 | PM traceability output | All prior tasks; evidence and unresolved-debt package complete |

### 5.4 Review tasks

| ID | Trigger | Acceptance |
|---|---|---|
| V-01 | After B-02 and protocol changes | No unauthorised schema drift |
| V-02 | After B-06/B-07 | Layering respected; no critical architecture issue |
| V-03 | After B-09 | Retry, recovery, and safety meet specification |
| V-04 | After B-10 | Full release sign-off package |

### 5.5 Quality gates and retained artefacts

- Requirements parity: each functional/non-functional requirement mapped to implementation and test evidence.
- Schema parity: firmware-aligned fields preserved end to end.
- Reliability: connectivity loss, retries, and recovery validated.
- Security/safety: destructive actions, stale commands, and replay protected.
- Observability: logs and diagnostics sufficient for field incident triage.

Retained PM artefacts were the decision log, risk register, open-issues register, requirement traceability matrix, reviewer findings, build/test instructions, and release-readiness checklist.

Weekly status used five sections: completed with evidence, in progress with blockers, risks/mitigations, decisions needed with deadlines, and next task/role assignments.

Final handoff required all task cards closed with evidence, all quality gates passed, reviewer sign-off, current PM governance artefacts, and an approved release checklist.

## 6. Consolidation record

The following historical files were merged into this archive on 2026-07-17:

- `docs/legacy-agent-workflow/README.md`
- `docs/legacy-agent-workflow/ai-builder.agent.md`
- `docs/legacy-agent-workflow/orchestrator.agent.md`
- `docs/legacy-agent-workflow/planner.agent.md`
- `docs/legacy-agent-workflow/designer.agent.md`
- `docs/legacy-agent-workflow/coder.agent.md`
- `docs/legacy-agent-workflow/fast-coder.agent.md`
- `docs/legacy-agent-workflow/reviewer.agent.md`
- `docs/legacy-agent-workflow/documentation.agent.md`
- `docs/legacy-agent-workflow/gitops.agent.md`
- `docs/legacy-agent-workflow/ORCHESTRATOR_WORK_PACKAGE.md`

The consolidation intentionally retains behavioural contracts and the native-app task model while removing editor-specific tool arrays, repeated archive notices, repeated response boilerplate, and superseded model routing.
