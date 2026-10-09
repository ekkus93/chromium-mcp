# Chromium MCP Agent Browser Handoff

Updated: 2026-10-09

## Current project state

The project embeds an opt-in MCP endpoint in Chromium's browser process. The
endpoint and browser control tools use Chromium-native APIs. The event-stream
and subresource network capture work is now in the source tree and has been
validated against the local browser fixture. No CDP or sidecar is used.

The working branch is `master`, based on `8f4fe4e54b` before this handoff
commit. The 2026-10-09 source and documentation changes, including this file,
are intended to be committed and pushed together. Local output files under
`out/Default/` are not part of the commit.

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
- Updated the canonical TODO with the validation results below.

## Validation

Run on 2026-10-09 in this checkout:

- `autoninja -j1 -C out/Default chrome` completed successfully; Ninja reported
  no pending work.
- `npm test --prefix agent_protocol` passed: 2 tests.
- `taskset -c 0 out/Default/unit_tests --gtest_filter='BrowserAgent*'` passed:
  8 tests.
- Python syntax compilation for
  `agent_protocol/tests/chromium_mcp_integration.py` passed.
- `git diff --check` passed.
- The MCP browser integration suite reached and passed subresource request and
  response header checks and decoded the captured response body successfully.
  The overall run then failed at cross-origin iframe evaluation: `page.evaluate`
  returned `null` for `document.body.textContent` in a live, committed child
  frame at `http://localhost:<fixture-port>/cross`. This remains to diagnose;
  the complete browser integration suite has not passed.

The headless Ubuntu session has no controllable desktop permission settings.
Real OS/device permission denial remains unverified. On the development
computer with a supported desktop, change an OS-level camera, microphone, or
location permission to denied while leaving the browser content setting
granted, then run the MCP browser integration permission check and record the
OS/version, permission changed, and observed `browser_status`,
`combined_status`, and `os_or_device_denied` values. The relevant existing test
is in `agent_protocol/tests/chromium_mcp_integration.py` around the
`page.permission_status` assertions.

## Remaining work

1. Diagnose and fix the cross-origin child-frame `page.evaluate` returning
   `null`; rerun the complete MCP browser integration suite.
2. On a desktop OS with controllable permission settings, verify a real
   OS/device-level denial while the browser-level permission is granted, then
   record platform and evidence in the TODO.
3. Rerun `npm test --prefix agent_protocol`, the focused Chromium unit tests,
   and `autoninja -j1 -C out/Default chrome` after any fixes. Update the TODO
   only to reflect the final results.
4. Review the full local diff before further commits. Keep generated build
   output and `.serena/` local state out of Git.

## Useful commands

From the repository root, use the checkout's `depot_tools` on `PATH`:

```sh
taskset -c 0 env PATH="$PWD/third_party/depot_tools:$PATH" \
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
