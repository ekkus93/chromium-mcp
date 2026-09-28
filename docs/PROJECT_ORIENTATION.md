# Chromium MCP Project Orientation

Status: Phase 0 orientation  
Companion spec: `docs/CHROMIUM_MCP_AGENT_BROWSER_SPEC.md`  
Canonical TODO: `docs/CHROMIUM_MCP_AGENT_BROWSER_TODO.md`

## Repository shape

This repository is a Chromium source checkout/fork. The agent-browser work should
stay isolated and rebaseable. Do not scatter agent-specific code through broad
Chromium subsystems unless a later implementation phase proves that a narrow hook
is required.

Important existing Chromium areas:

- `chrome/`: desktop Chrome/Chromium browser code. The planned browser-agent
  service should live under a clearly named subtree such as `chrome/browser/agent/`.
- `content/`: browser core, navigation, frames, WebContents integration, and
  process separation. Agent code should treat renderer-provided data as
  untrusted when interacting with this layer.
- `components/`: shared Chromium feature components. Use only when the agent
  feature needs reusable non-UI logic.
- `base/`: common Chromium infrastructure. Avoid project-specific policy here.
- `third_party/`: vendored dependencies. Do not add MCP/sidecar dependencies
  here until the dependency and license plan is explicit.
- `docs/`: Chromium documentation plus Chromium MCP project docs.
- `.github/workflows/`: GitHub Actions workflows. At the time this document was
  added, this fork only had a PR-closing workflow and no docs/sidecar/Chromium
  validation workflow.

## Upstream sync and rebase workflow

Chromium is normally managed with `depot_tools`, `gclient`, `gn`, and `ninja`.
For this fork, keep custom work small and concentrated so an upstream Chromium
rebase can be reviewed as:

1. Sync/update the Chromium checkout using the repository owner's preferred
   upstream mechanism.
2. Rebase the small Chromium MCP patch set on top of the updated Chromium tree.
3. Resolve conflicts only in the isolated agent areas first.
4. Re-run the narrowest relevant GN/Ninja targets before broader browser tests.
5. Update this orientation document if upstream layout changes affect planned
   agent integration points.

Do not vendor generated `out/` content, local `args.gn`, build products, traces,
logs, or local credentials.

## Branch and commit policy

The current project working model is direct commits to `master` for coherent
vertical slices. Do not create a new branch or pull request per task or subtask.
For large Chromium changes, keep one direct-to-`master` commit focused around a
single vertical slice whenever possible: service skeleton, protocol scaffolding,
sidecar, policy, audit, tracing, or a single tested browser capability.

If a later repository policy requires branches for a high-risk or conflict-prone
change, document the exception in the TODO before using it. The default remains
no branch/PR churn.

## Supported initial development hosts

Initial development targets desktop Chromium first:

- Primary: Linux desktop Chromium development.
- Secondary/future: macOS and Windows support after the browser-agent service
  interface stabilizes.
- Out of scope for the first MVP: Android, iOS, ChromeOS-specific integration,
  and WebView-specific integration unless needed by Chromium build plumbing.

## Build arguments

No project-specific `out/*/args.gn` is committed. Chromium build arguments are
local developer state. Start from the normal Chromium desktop build flow and add
agent-specific flags only after Phase 1 introduces them.

Typical local Linux development setup starts with:

```sh
gn gen out/Default
gn args out/Default
autoninja -C out/Default chrome
```

Example debug-oriented local args, to be adjusted by the developer:

```gn
is_debug = true
symbol_level = 1
is_component_build = true
use_remoteexec = false
```

Do not treat this example as a repository-wide required configuration.

## Primary GN/Ninja targets

The exact target list will evolve as agent code lands. Initial target guidance:

- `chrome`: full desktop browser build for manual smoke tests.
- `unit_tests`: broad unit-test binary, useful after adding non-UI components.
- `browser_tests`: browser integration tests for WebContents, tabs, navigation,
  permissions, dialogs, downloads, and UI-mediated flows.
- Focused future targets under `chrome/browser/agent/` once Phase 1 creates the
  agent service skeleton.

Prefer the narrowest target that covers the changed area, then widen validation
when behavior crosses browser/profile/WebContents boundaries.

## CI capacity

Current observed repository CI capacity is minimal. The `.github/workflows/`
directory currently contains only `close-pull-request.yml`. There is no verified
workflow yet for docs linting, sidecar tests, protocol schema validation,
Chromium agent unit tests, browser tests, or end-to-end fixture tests.

Phase 25 must add CI coverage. Until then, document local validation commands in
commit messages and in this project's qualification notes.

## Phase 0 status

This document, together with `README.md`, `docs/SECURITY_MODEL.md`,
`docs/TOOL_CONTRACT.md`, and `docs/TEST_PLAN.md`, establishes the initial
orientation and scaffolding required before the browser-service implementation
begins.
