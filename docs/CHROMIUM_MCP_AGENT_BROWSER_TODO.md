# Chromium MCP Agent Browser TODO

Status: Linux implementation complete; macOS, Windows, and ChromeOS permission verification is deferred
Companion spec: `docs/CHROMIUM_MCP_AGENT_BROWSER_SPEC.md`

## Working rules

- MCP runs in Chromium's browser process and uses native Chromium APIs.
- Do not use CDP, DevTools remote debugging, Playwright, or a sidecar to control Chromium.
- Do not add agent-specific policy allowlists, page modes, redaction, approval gates, or capability tiers.
- Keep the endpoint local and opt-in. Preserve Chromium and operating-system security boundaries.
- Continue using Chromium's existing build and test tools; keep project code under `chrome/browser/agent/`.

## Phase 0 — Repository orientation and scaffolding

- [x] Document repository layout, Chromium build args, and contributor workflow.
- [x] Document supported development host and build/test targets.
- [x] Add project README links to the specification and TODO.
- [x] Add initial security, tool-contract, and test-plan documents.

## Phase 1 — Browser service skeleton

- [x] Add disabled-by-default `BrowserAgentService` feature and explicit runtime opt-in.
- [x] Add profile-scoped `BrowserAgentService` and browser lifetime wiring.
- [x] Add optional audit and trace output path configuration.
- [x] Add startup/shutdown logging and service unit tests.
- [x] Build Chromium and run focused service tests.

## Phase 2 — In-process MCP endpoint

- [x] Select and document MCP Streamable HTTP revision `2026-07-28`.
- [x] Implement an HTTP endpoint owned by the browser process and bound to loopback.
- [x] Authenticate the local MCP client with a per-profile token, without restricting its browser tools.
- [x] Reject requests with a web Origin and validate loopback peer and Host.
- [x] Implement MCP JSON-RPC handling, discovery, `tools/list`, and the initial `browser.version` `tools/call`.
- [x] Cancel in-flight HTTP requests when the client closes the request connection.
- [x] Implement structured JSON-RPC and tool error mapping with stable tool error codes.
- [x] Add MCP JSON-RPC request/response schema tests against the selected revision.
- [x] Add endpoint integration tests for authentication, origin rejection, and malformed headers.

### Phase 2 acceptance

- [x] MCP discovery, listing, and tool invocation work directly over the local endpoint.
- [x] Requests without the per-profile token and requests carrying a web Origin are rejected.
- [x] The endpoint does not expose CDP or a second remote-debugging service.

## Phase 3 — Browser/context/page registries

- [x] Implement context registry mappings and opaque context handles.
- [x] Map existing regular and off-the-record profiles to contexts.
- [x] Implement context create/list/close and native cookie read/write tools.
- [x] Implement page registry mappings backed by native tab handles and opaque page handles.
- [x] Enumerate native tab strips so page listing discovers tabs and popups; reject closed TabHandles.
- [x] Validate push notifications for tab creation, closure, popups, and navigation lifecycle in the browser integration suite.
- [x] Implement page list/new/close/focus/info tools.
- [x] Add context/page creation, closure, and stale-handle integration tests.

## Phase 4 — Direct page control

- [x] Implement navigation, reload, history, URL commit waits, and load completion waits.
- [x] Stop native navigation/load waiters when their HTTP request is cancelled.
- [x] Implement direct main-world JavaScript evaluation through a native RenderFrameHost API, not CDP.
- [x] Enumerate current frames and target JavaScript evaluation at a selected frame.
- [x] Expose direct DOM observation and mutation through unrestricted page evaluation.
- [x] Capture native accessibility tree snapshots across page frames.
- [x] Read visible text, headings, and links from the page main world.
- [x] Observe console messages through `WebContentsObserver` and expose polling.
- [x] Capture page screenshots through native surface copying and return MCP image content.
- [x] Implement MCP resource listing and reads for browser version and contexts.
- [x] Send coordinate-based clicks as native Chromium mouse events.
- [x] Send coordinate-based hover as native Chromium mouse movement.
- [x] Send text through the native keyboard character event path.
- [x] Fill CSS-selected input, textarea, and contenteditable elements.
- [x] Focus CSS-selected elements and select options by value through direct page evaluation.
- [x] Check and uncheck CSS-selected checkboxes with native mouse input.
- [x] Send common named key presses through native Chromium keyboard events.
- [x] Find visible elements by CSS selector, role, accessible name, and text.
- [x] Expose native accessibility descriptions, placeholders, expanded/collapsed and required states, restrictions, checked states, hierarchy, and collection position data.
- [x] Target locators within any enumerated child frame through its frame handle.
- [x] Add browser integration tests for normal pages, frames, navigation, evaluation, and input.

## Phase 5 — Full browser capabilities

- [x] Accept or dismiss active JavaScript dialogs and override prompt text through Chromium's dialog manager.
- [x] Expose dialog details and handle beforeunload dialogs through native managers.
- [x] List downloads through the native context download manager.
- [x] Start frame-associated downloads through Chromium's native download path.
- [x] Cancel active downloads through Chromium's native download item.
- [x] Wait for native download terminal states.
- [x] Add native file chooser upload preparation and selection tools.
- [x] Read and set common site permissions through Chromium's content settings map.
- [x] Expose registered native content settings and combined OS/device permission status.
- [x] Test combined browser and OS/device permission status and distinguish browser-level denial.
- [x] Integrate Linux GNOME location, camera, and microphone privacy settings through GSettings; retain the default allow behavior when those schemas are unavailable.
- [x] Verify real OS-level permission denial on Linux GNOME with the browser site permission granted; record platform and status evidence below.
- [x] Add requested locale, user-agent, and viewport controls through native Chromium preferences and `WebContents` APIs.
- [x] Add a native per-page user-agent override using `WebContents::SetUserAgentOverride`.
- [x] Read and write local and session storage for the current page origin.
- [x] Enumerate and clear native local storage keys across context origins.
- [x] Import and export local/session storage for the current document or a selected cross-origin frame using native frame execution.
- [x] Inspect context cookies, current-page local/session storage, and native local-storage origins/partition keys/sizes.
- [x] Observe navigation request/response lifecycle and completed resource loads; clear the native HTTP cache.
- [x] Validate subresource request/response header and body capture in the browser suite; native interception, stream teeing, and integration assertions are implemented.
- [x] Capture main-frame navigation request and response headers through `NavigationHandle`.
- [x] Expand native resource events with request identity, destination, referrer, network access, and received-byte metadata.
- [x] Add native context controls for Accept-Language, referrer headers, and idle connections.
- [x] Append optional metadata-only MCP request audit events to a configured JSONL path.
- [x] Implement optional Chromium tracing output.
- [x] Add integration coverage for the main native API families: page lifecycle, accessibility, input, storage, downloads/uploads, dialogs, permissions, network, tracing, and audit.

## Phase 6 — Qualification and documentation

- [x] Add deterministic local browser fixtures and an MCP client integration suite.
- [x] Verify current tools use native Chromium APIs and no CDP path exists.
- [x] Verify authenticated tools expose the full documented browser capability surface.
- [x] Document endpoint setup, supported tools, build/test commands, and limitations imposed by Chromium/OS.
- [x] Document local schema, unit, build, and browser integration validation commands; project CI is intentionally not configured.
- [x] Keep Chromium rebase instructions and known upstream conflict areas current.

## MVP completion criteria

- [x] Rebuild Chromium with the latest event-stream and subresource capture hooks.
- [x] An MCP client directly controls tabs through Chromium-native APIs.
- [x] Navigation, observation, evaluation, input, dialogs, downloads, and permissions work in browser integration tests.
- [x] No CDP, Playwright, sidecar, agent allowlist, redaction layer, or approval gate is required for current control tools.
- [x] Rerun MCP and browser integration tests for the latest implementation, and confirm the documentation matches.

## Latest validation (2026-10-09)

- `taskset -c 0 env PATH="/home/phil/depot_tools:$PATH" autoninja -j1 -C
  out/Default chrome` completed successfully.
- `npm test --prefix agent_protocol` passed (2 tests).
- `taskset -c 0 out/Default/unit_tests --gtest_filter='BrowserAgent*'`
  passed (8 tests).
- `xvfb-run -a taskset -c 0 out/Default/unit_tests
  --gtest_filter='BrowserAgent*:ChromePermissionsClientTest.*'` passed (17 tests).
- `taskset -c 0 env CHROME_BIN=../out/Default/chrome npm run test:chromium
  --prefix agent_protocol` passed, including event streaming, subresource
  request/response headers and response body bytes, and cross-origin iframe
  evaluation.
- The earlier failures were from running the pre-pull local Chrome binary.
  Rebuilding the current source resolved both the event-stream 403 and iframe
  evaluation returning `null`; no source behavior change was needed.
- The Linux permission handler reads GNOME's `org.gnome.system.location`
  `enabled` setting and the `org.gnome.desktop.privacy` `disable-camera` and
  `disable-microphone` settings. It observes changes and opens GNOME Privacy
  settings when available. Linux desktops without these schemas retain the
  allow-by-default behavior.
- `CHROMIUM_MCP_EXPECT_OS_PERMISSION_DENIAL` makes the browser integration suite
  require the browser permission to be granted and the combined status to be
  denied, then prints the platform/status evidence.
- With GNOME Location disabled, the geolocation integration run passed on
  `Linux-7.0.0-34-generic-x86_64-with-glibc2.39` with
  `browser_status=granted`, `combined_status=denied`,
  `os_or_device_denied=true`.
- With GNOME's camera and microphone privacy switches disabled, their
  integration runs passed on the same platform with the same granted/denied/
  true status values. The original camera and microphone switches were restored
  to `false` after the runs.
- `python3 -m py_compile agent_protocol/tests/chromium_mcp_integration.py`
  passed; `git diff --check` passed.
