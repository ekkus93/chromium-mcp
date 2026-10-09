# Chromium MCP Agent Browser Test Plan

Status: Protocol schema passes; browser integration is being updated for the 2026-07-28 transport and rebuilt implementation
Companion spec: `docs/CHROMIUM_MCP_AGENT_BROWSER_SPEC.md`  
Canonical TODO: `docs/CHROMIUM_MCP_AGENT_BROWSER_TODO.md`

## Test strategy

The project should validate behavior at the narrowest layer that can prove the
claim, then add browser/end-to-end coverage for cross-boundary behavior.

Planned layers:

1. Documentation checks.
2. Protocol schema validation.
3. Chromium C++ unit tests for MCP parsing, browser registries, and native API adapters.
4. Chromium browser tests for tabs, navigation, WebContents integration,
   permissions, dialogs, downloads/uploads, and native API behavior.
5. Deterministic local MCP client tests against browser fixtures.

## Local Chromium build entry points

No repository-specific GN args are committed yet. Use normal Chromium setup and
configure a local output directory.

On this development host, pin each Chromium build and browser run to one CPU
core, and keep Ninja to one local job. Do not start a build while the CPU is
already hot or another compile is active. Common Linux commands:

```sh
gn gen out/Default
gn args out/Default
taskset -c 0 env PATH=/home/phil/depot_tools:$PATH \
  autoninja --fast_local -j1 -C out/Default chrome
```

Initial broad test targets:

```sh
taskset -c 0 autoninja -j1 -C out/Default unit_tests
taskset -c 0 autoninja -j1 -C out/Default browser_tests
taskset -c 0 out/Default/unit_tests --gtest_filter='*Agent*'
taskset -c 0 out/Default/browser_tests --gtest_filter='*Agent*'
```

The `*Agent*` filters select the browser-agent test suites.

## MCP endpoint test entry points

Protocol schema checks run from `agent_protocol/`:

```sh
cd agent_protocol
npm ci
npm test
```

The endpoint integration check launches a built headless browser and verifies
discovery, authentication, origin/header rejection, regular and private
contexts, stale handles, navigation waits, main-world evaluation in the main
frame and an iframe, regular/private cookie read/write, native click, hover,
checkbox and typing input, element focus and option selection, native
accessibility snapshots, visible content, console messages, MCP screenshots,
authenticated POST `subscriptions/listen` streams and cursor reads for tab and
popup creation/closure and navigation,
regular/private geolocation content setting isolation, locale, user-agent and
viewport emulation, page and native context storage operations, file input
selection, prompts and beforeunload dialogs, registered content settings,
combined OS/device permission status, navigation and resource events including
subresource request/response headers and bodies, HTTP cache clearing, native tracing,
optional audit logging, and download completion,
cancellation, and terminal-state waits:

```sh
cd agent_protocol
taskset -c 0 env CHROME_BIN=../out/Default/chrome npm run test:chromium
```

For an independently maintained client check, install MCP Python SDK 2.3.0 in
an isolated environment and set `CHROMIUM_MCP_SDK_PYTHONPATH` to that
environment's package root when running the same command. The integration
suite then uses the SDK's Streamable HTTP client for discovery, tool listing,
tool invocation, and resource reads.

To verify a real OS/device permission denial, keep the fixture site's Chromium
permission allowed and disable a system permission in the OS privacy settings.
The browser integration suite accepts
`CHROMIUM_MCP_EXPECT_OS_PERMISSION_DENIAL` set to `camera`, `microphone`, or
`geolocation`. It requires `browser_status=granted`, `combined_status=denied`,
and `os_or_device_denied=true`, then prints a JSON evidence record with the
platform and observed statuses.

On Linux desktops with GNOME settings schemas, the browser reads
`org.gnome.system.location enabled`,
`org.gnome.desktop.privacy disable-camera`, and
`org.gnome.desktop.privacy disable-microphone`. Disable the matching setting in
GNOME Privacy & Security before running the suite. Other Linux desktops without
these schemas retain the default device-permission behavior. macOS, Windows,
and ChromeOS continue using their native permission integrations.

## Required fixture coverage

The Phase 6 fixture server should include deterministic local pages for:

- simple navigation;
- search form;
- login form;
- password field;
- payment-like form;
- account-change form;
- iframe content;
- popup/new-window;
- JavaScript dialogs;
- downloads;
- uploads;
- network requests;
- console logging.

## Core acceptance tests by feature area

- Endpoint: standard MCP discovery/list/call flows work; cross-origin browser
  requests and unauthenticated local callers fail.
- Native control: tab lifecycle, navigation, evaluation, input, accessibility,
  downloads/uploads, permissions, storage, and network tools invoke Chromium
  APIs directly, without CDP.
- Tool access: the authenticated caller can use every documented tool; no
  agent-specific allowlist or approval gate blocks browser operations.
- Errors: malformed arguments, stale handles, timeouts, and native Chromium or
  OS errors produce useful MCP errors.
- Optional audit/trace: recording reflects operations without changing or
  redacting their results.

## Local validation

This project does not configure a GitHub Actions workflow. Run protocol and
browser integration checks locally with the commands above. Keep Chromium build
and browser test processes pinned to one CPU core on this development host.

## Phase 0 validation

The documentation-only Phase 0 slice was validated by repository inspection and
manual review. No automated CI check currently proves it.

## Phase 1 validation

`//chrome/browser/agent:unit_tests` covers the feature and explicit-switch gate,
artifact path parsing and service shutdown. Build and run it through the regular Chromium unit test target:

```sh
taskset -c 0 autoninja -j1 -C out/Default unit_tests
taskset -c 0 out/Default/unit_tests --gtest_filter='BrowserAgentServiceTest.*'
```
