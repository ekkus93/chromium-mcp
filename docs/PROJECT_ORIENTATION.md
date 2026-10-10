# Chromium MCP Project Orientation

Status: Embedded MCP endpoint and 2026-10-09 Linux remediation and review follow-up qualification recorded
Companion spec: `docs/CHROMIUM_MCP_AGENT_BROWSER_SPEC.md`  
Canonical TODO: `docs/CHROMIUM_MCP_AGENT_BROWSER_TODO.md`
Current remediation: `docs/CHROMIUM_MCP_AGENT_BROWSER_REMEDIATION_TODO.md`
Review follow-up results: `docs/CHROMIUM_MCP_AGENT_BROWSER_REVIEW_FOLLOWUP_TODO.md`

## Repository shape

This repository is a Chromium source checkout/fork. The agent-browser work should
stay isolated and rebaseable. Do not scatter agent-specific code through broad
Chromium subsystems unless a later implementation phase proves that a narrow hook
is required.

Important existing Chromium areas:

- `chrome/`: desktop Chrome/Chromium browser code. The in-process MCP service
  and native tools live under `chrome/browser/agent/`.
- `content/`: browser core, navigation, frames, WebContents integration, and
  process separation. Agent code should treat renderer-provided data as
  untrusted when interacting with this layer.
- `components/`: shared Chromium feature components. Use only when the agent
  feature needs reusable non-UI logic.
- `base/`: common Chromium infrastructure. Avoid project-specific policy here.
- `third_party/`: vendored Chromium dependencies. MCP request handling is
  implemented in `chrome/browser/agent/`, without an external sidecar.
- `docs/`: Chromium documentation plus Chromium MCP project docs.
- `.github/workflows/`: GitHub Actions workflows. At the time this document was
  added, this fork only had a PR-closing workflow and no docs, MCP protocol, or Chromium
  validation workflow.

## Upstream sync and rebase workflow

Chromium is normally managed with `depot_tools`, `gclient`, `gn`, and `ninja`.
For this fork, add the canonical Chromium upstream remote and update the pinned
Chromium revision/dependencies through the normal `depot_tools` workflow. Keep
custom work small and concentrated so an upstream rebase can be reviewed as:

1. Fetch the selected upstream Chromium revision and update the checkout's
   pinned revision/dependencies.
2. Rebase the small Chromium MCP patch set on top of the updated Chromium tree.
3. Resolve conflicts only in the isolated agent areas first.
4. Re-run the narrowest relevant GN/Ninja targets before broader browser tests.
5. Update this orientation document if upstream layout changes affect planned
   agent integration points.

Current upstream conflict points are `chrome/browser/BUILD.gn` (agent target
membership), `chrome/browser/profiles/chrome_browser_main_extra_parts_profiles.cc`
(profile lifetime hook), `chrome/common/chrome_features.{h,cc}` and
`chrome/common/chrome_switches.h` (opt-in surface), and
`content/browser/renderer_host/render_frame_host_impl.{h,cc}` (the narrow
main-world evaluation hook). Network observation also touches
`chrome/renderer/url_loader_throttle_provider_impl.{h,cc}`,
`chrome/browser/chrome_content_browser_client.cc`,
`chrome/browser/chrome_content_browser_client_receiver_bindings.cc`, and
`chrome/common/agent/`; include those hooks in rebase reviews. Review those diffs first after a rebase. The
new public declaration in `content/public/browser/browser_automation.h` and
the isolated `chrome/browser/agent/` sources should usually apply without
cross-area conflict. Rebuild with the single-core command below and rerun the
focused browser integration suite before updating the TODO status.

Do not vendor generated `out/` content, local `args.gn`, build products, traces,
logs, or local credentials.

## Branch and commit policy

The current project working model is direct commits to `master` for coherent
vertical slices. Do not create a new branch or pull request per task or subtask.
For large Chromium changes, keep one direct-to-`master` commit focused around a
single vertical slice whenever possible: service skeleton, embedded MCP transport, browser registries, tracing, or a
single tested browser capability.

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
local developer state. The initial Linux desktop build in this checkout used:

```gn
is_debug = false
is_component_build = true
symbol_level = 0
```

These args reduce local build cost; they are not a required repository-wide
configuration. There are no agent-specific build args yet.

Typical local Linux development setup starts with:

```sh
gn gen out/Default
gn args out/Default
```

Build with the one-core command below so Ninja and its child processes stay
within the host's thermal limit.

On the current 16-core development workstation, cap build load to reduce
thermal stress. Keep both the build scheduler and the entire process tree on
one core and use one local Ninja job. Do not start a build while the CPU is hot
or another compile is active:

```sh
taskset -c 0 env PATH=/home/phil/depot_tools:$PATH \
  autoninja --fast_local -j1 -C out/Default chrome
```

Use the same `taskset -c 0` prefix for browser integration runs on this
machine. The integration client's HTTP timeout is 30 seconds to allow for
the lower CPU limit.

The initial build uses a lower-cost non-debug configuration; it is suitable
for smoke tests but does not provide full debug symbols:

```gn
is_debug = false
is_component_build = true
symbol_level = 0
```

Do not treat this example as a repository-wide required configuration. The
experimental endpoint requires `--enable-features=BrowserAgentService`,
`--enable-browser-agent-service`, and `--browser-agent-mcp-port=0` (or a chosen
port). For example:

```sh
out/Default/chrome --enable-features=BrowserAgentService \
  --enable-browser-agent-service --browser-agent-mcp-port=0
```

With `--user-data-dir=/tmp/chromium-mcp-profile`, read the endpoint URL and
bearer token from
`/tmp/chromium-mcp-profile/Default/BrowserAgentRuntime/browser-agent-mcp-endpoint`
and the adjacent `browser-agent-mcp-token` file. The port can be chosen with
`--browser-agent-mcp-port`; `0` asks the OS for an available loopback port.
The endpoint accepts authenticated MCP JSON-RPC `POST` requests with matching
`MCP-Protocol-Version: 2026-07-28` and `Mcp-Method` headers. `tools/call`
requests also need a matching `Mcp-Name` header. For example, after setting
`ENDPOINT` and `TOKEN` from those files:

```sh
curl -sS "$ENDPOINT" -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: tools/list' \
  --data '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'
```

The current tool list is maintained in [Tool Contract](TOOL_CONTRACT.md).
Linux is the tested development platform; macOS and Windows support remains
future work. Chromium's profile ownership, browser sandbox, site security
model, and OS permissions still apply to tool effects. Missing capability
families are listed in the [TODO](CHROMIUM_MCP_AGENT_BROWSER_TODO.md).

For local verification, run `npm test` in `agent_protocol/`, then run
`CHROME_BIN=../out/Default/chrome npm run test:chromium` there after building
Chrome. On this workstation, prefix the latter command with
`taskset -c 0`.

## Primary GN/Ninja targets

The exact target list will evolve as agent code lands. Initial target guidance
(build the binaries before running them):

- `chrome`: full desktop browser build for manual smoke tests.
- `unit_tests`: broad unit-test binary, useful after adding non-UI components.
- `browser_tests`: browser integration tests for WebContents, tabs, navigation,
  permissions, dialogs, downloads, and UI-mediated flows.
- Focused targets under `chrome/browser/agent/` as native tools are added.

Prefer the narrowest target that covers the changed area, then widen validation
when behavior crosses browser/profile/WebContents boundaries.

## CI capacity

Current observed repository CI capacity is minimal. The `.github/workflows/`
directory currently contains only `close-pull-request.yml`. There is no verified
workflow yet for docs linting, MCP protocol schema validation,
Chromium agent unit tests, browser tests, or end-to-end fixture tests. Docs
commits can be reviewed without CI; full Chromium tests need a Chromium-capable
runner with substantial disk and build time. Runner capacity has not been
verified.

CI coverage is not configured for the Chromium/MCP integration suite. Treat
local validation commands in the canonical TODO and remediation record as the
current qualification evidence; a future CI setup needs a Chromium-capable
runner with the required build and display support.

## Phase 0 status

This document, together with `README.md`, `docs/SECURITY_MODEL.md`,
`docs/TOOL_CONTRACT.md`, and `docs/TEST_PLAN.md`, establishes the initial
orientation and scaffolding required before the browser-service implementation
begins. Initial supported development is Linux desktop; macOS and Windows are
follow-up platforms after the service boundary stabilizes. Android, iOS,
ChromeOS-specific integration, and WebView are out of scope for the initial MVP.
