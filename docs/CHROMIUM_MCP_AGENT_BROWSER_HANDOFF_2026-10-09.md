# Chromium MCP Agent Browser Handoff

Updated: 2026-10-09

## Current project state

The project embeds an opt-in MCP endpoint in Chromium's browser process. The
endpoint and browser control tools use Chromium-native APIs. The event-stream
and subresource network capture work is now in the source tree and has been
validated against the local browser fixture. No CDP or sidecar is used.

The working branch is `master` at `abba3e88b9`. The local Chrome binary was
stale after the source update; rebuilding `out/Default/chrome` resolved the
event-stream and cross-origin iframe integration failures without source code
changes. Local output files under `out/Default/` are not part of the commit.

## Recent implementation

- Added browser-process `BrowserAgentNetworkEventHost` Mojo plumbing between
  renderer resource throttles and the profile-scoped browser-agent service.
- Moved the network throttle implementation into `chrome/common/agent/` so it
  can be used by the renderer. It reports subresource request and response
  headers and tees response bodies while forwarding them to the page.
- Added fixture assertions for request headers, response headers, request
  identity, and captured response body bytes.
- Updated the event assertions to distinguish `resource_complete` records from
  the newer request and response events.
- Updated `AGENTS.md` with the repository's Serena code navigation guidance.
- Added Linux integration for GNOME's system location, camera, and microphone
  privacy settings through GSettings.
- Updated the canonical TODO with the validation results below.

## Validation

Run on 2026-10-09 in this checkout after rebuilding the local browser:

- `taskset -c 0 env PATH="/home/phil/depot_tools:$PATH" autoninja -j1 -C
  out/Default chrome` completed successfully.
- `npm test --prefix agent_protocol` passed: 2 tests.
- `taskset -c 0 out/Default/unit_tests --gtest_filter='BrowserAgent*'` passed:
  8 tests.
- `taskset -c 0 env CHROME_BIN=../out/Default/chrome npm run test:chromium
  --prefix agent_protocol` passed twice, including event streaming, subresource
  request/response headers and response body bytes, and cross-origin iframe
  evaluation.
- The opt-in OS-denial browser integration run passed for geolocation with
  GNOME Location disabled, camera with `disable-camera=true`, and microphone
  with `disable-microphone=true`. Each reported
  `browser_status=granted`, `combined_status=denied`, and
  `os_or_device_denied=true` on Linux GNOME. Camera and microphone settings were
  restored to their original `false` values afterward.
- `git diff --check` passed.

The earlier event-stream 403 and iframe evaluation returning `null` came from
running the pre-pull local Chrome binary. The suite passed after rebuilding the
current source, with no source behavior change.

The opt-in permission check is controlled by
`CHROMIUM_MCP_EXPECT_OS_PERMISSION_DENIAL` and prints platform and status
evidence. See the TODO and test plan for Linux GNOME settings and other
platform-specific setup.

## Remaining work

1. Review the full local diff before further commits. Keep generated build
   output and `.serena/` local state out of Git.

## Useful commands

From the repository root, use the checkout's `depot_tools` on `PATH`:

```sh
taskset -c 0 env PATH="/home/phil/depot_tools:$PATH" \
  autoninja -j1 -C out/Default chrome
taskset -c 0 out/Default/unit_tests --gtest_filter='BrowserAgent*'
npm test --prefix agent_protocol
taskset -c 0 env CHROME_BIN=../out/Default/chrome npm run test:chromium \
  --prefix agent_protocol
```

The canonical status checklist is
[`CHROMIUM_MCP_AGENT_BROWSER_TODO.md`](CHROMIUM_MCP_AGENT_BROWSER_TODO.md),
and the project validation commands are in [`TEST_PLAN.md`](TEST_PLAN.md).

## Rebase notes

The latest network capture changes touch shared Chromium integration points:
`chrome/browser/chrome_content_browser_client.cc`,
`chrome/browser/chrome_content_browser_client_receiver_bindings.cc`,
`chrome/common/BUILD.gn`, and
`chrome/renderer/url_loader_throttle_provider_impl.{cc,h}`. These are likely
upstream rebase conflict areas. The network hook is delivered through a
browser-owned Mojo interface; preserve process/frame validation in
`BrowserAgentNetworkEventHost` when rebasing.
