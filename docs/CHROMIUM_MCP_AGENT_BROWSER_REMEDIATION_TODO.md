# Chromium MCP Agent Browser Remediation TODO

Status: Complete on Linux; cross-platform OS permission verification is deferred
Spec: [Remediation Specification](CHROMIUM_MCP_AGENT_BROWSER_REMEDIATION_SPEC.md)
Source checklist: [Agent Browser TODO](CHROMIUM_MCP_AGENT_BROWSER_TODO.md)

Each checkbox requires implementation and the stated evidence. Keep completed results and any design decision in this file.

## 0. Establish the baseline

- [x] Record the current source revision, GN args, and rebuilt Chrome binary revision used for validation.
- [x] Convert each review finding into a regression case before marking its section complete; use the published MCP `2026-07-28` schemas and transport rules as external authority.

## 1. Network capture is opt-in and bounded — spec R1

- [x] Gate renderer throttle creation on both the feature and runtime opt-in, with no agent interception in disabled browsing. (A separate no-opt-in Chrome run completed a 1 MiB page fetch and created no MCP endpoint artifacts.)
- [x] Confirm the gate and page/context association for regular, private, and worker traffic; keep process/frame validation at the Mojo host. (Regular and off-the-record page association are covered. Off-the-record services resolve through the original profile; Blink uses separate worker throttle providers, intentionally excluded because they have no page-frame association.)
- [x] Set and document per-response and aggregate memory budgets, truncation metadata, and the full-body retrieval contract.
- [x] Bound response tee buffering, Base64/event copies, and queued network events without altering bytes delivered to the page. (The page receives the full 1 MiB fixture; capture remains limited to 256 KiB, and forwarding/capture/event queue budgets are documented.)
- [x] Cover small, oversized, slow-consumer, cancelled, redirected, and continuous-response fixtures; measure or assert the documented memory bound. (Redirected bodies are forwarded without the tee and report `capture_skipped`.)
- [x] Verify disabled normal browsing does not instantiate the agent response tee. (Verified by the no-opt-in transfer regression plus the feature-and-switch renderer gate.)

## 2. MCP transport matches revision 2026-07-28 — spec R2

- [x] Replace the standalone GET event stream with POST `subscriptions/listen` under the revision's published request and response schemas.
- [x] Choose and document a browser-events resource or negotiated extension; do not send arbitrary `notifications/message` events on the core listen stream.
- [x] Route browser change events only to matching active subscriptions, with acknowledgment and required subscription ID metadata; keep request-scoped notifications on their own response streams.
- [x] Stop watches and writes when a listen response disconnects; handle concurrent listeners and browser shutdown. (The integration suite exercises concurrent event delivery, listener disconnect, and SSE stream closure on browser shutdown.)
- [x] Return `405` for GET in the selected revision and the specified HTTP status and JSON-RPC error for unknown methods.
- [x] Update discovery, protocol schema, project client, fixtures, tool contract, and setup examples.
- [x] Verify interoperability with an independently maintained conforming client or published conformance fixtures, as well as the local integration client. (MCP Python SDK 2.3.0 smoke test passed.)

## 3. MCP HTTP validation is complete — spec R3

- [x] Require and type-check per-request `_meta` version and client capabilities; validate optional client identity when present and return HTTP 400 with `-32602` for missing mandatory fields.
- [x] Compare body and header versions; return `UnsupportedProtocolVersionError` with supported versions where required.
- [x] Validate `Mcp-Method` and decode/compare plain and Base64-sentinel `Mcp-Name` values.
- [x] Return HTTP 400 with JSON-RPC `-32020` for missing, malformed, or mismatched mirrored headers.
- [x] Validate content type, JSON-RPC shape, and notification behavior while preserving Origin, Host, loopback peer, and bearer-token enforcement.
- [x] Add positive and negative protocol cases and confirm invalid requests cause no browser operation.

## 4. Created contexts are independent — spec R4

- [x] Record the Chromium-supported design for separate ephemeral contexts that can host browser windows; account for `OTRProfileID::AllowsBrowserWindows()` restrictions.
- [x] Make each successful `browser.create_context` return a distinct owned context with isolated cookies, storage, permissions, pages, and downloads.
- [x] Distinguish owned contexts from discovered private profiles and prevent `browser.close_context` from destroying unrelated existing private windows.
- [x] Close owned context windows safely, destroy their profile at the right lifecycle point, and invalidate context/page/frame/observer handles.
- [x] Add concurrent-context isolation, close-one-keeps-other, existing-private-session, and stale-handle browser scenarios. (A separate `--incognito` browser run verifies that closing a discovered pre-existing private context fails and the context remains listed.)

## 5. Element tools report real state — spec R5

- [x] Make `page.fill` reject absent evaluation results, exceptions, unsupported/read-only controls, and failed value readback.
- [x] Use Chromium accessibility data or equivalent accessible-name rules for role/name queries; document match behavior. (The query scans all selector matches and computes names from `aria-labelledby`, `aria-label`, associated labels, image alternatives, native input values, visible contents, and title fallback; substring matching is case-sensitive.)
- [x] Define and implement viewport visibility and actionability, including hidden, offscreen, and obscured elements.
- [x] Document coordinate spaces and handle child-frame coordinates consistently with native pointer tools.
- [x] Add labeled-control, `aria-labelledby`, visibility, frame, and failed-fill regression fixtures.

## 6. Documentation and qualification — spec R6

- [x] Update `PROJECT_ORIENTATION.md` status and rebase conflict list for the renderer and common network hooks.
- [x] Update `TOOL_CONTRACT.md`, `TEST_PLAN.md`, and endpoint examples for POST subscriptions, context behavior, and capture limits.
- [x] Identify the older handoff as historical and reconcile current status statements across project entry points.
- [x] Rebuild Chromium and run focused C++ tests, protocol/schema checks, and browser integration against that rebuilt binary.
- [x] Record the disabled-feature, large-response, context-isolation, protocol-conformance, and element-result evidence here, including platform and source revision.
- [x] Update the canonical TODO with the corrected qualification status and link to this remediation work.

## Completion record

- Source revision: `0b194ec297205c250cd42c4579ec556ae28513e3` (working tree contains the remediation changes; rebuilt binary includes those changes).
- GN args: `is_debug = false`, `is_component_build = true`, `symbol_level = 0`.
- Chrome binary: `Chromium 156.0.8076.0`, Linux x86_64 (`Linux kawa 7.0.0-34-generic`).
- Build: `taskset -c 0 autoninja --fast_local -j1 -C out/Default chrome` passed after the network tee changes.
- Tests: `npm test --prefix agent_protocol` (2 passed); Python compilation for the integration harness, fixtures, and SDK smoke test passed; `git diff --check` passed; `out/Default/unit_tests --gtest_filter='BrowserAgent*'` passed (8 tests); the browser integration passed against the rebuilt binary, including disabled browsing, small/large/slow/cancelled/redirected transfers, contexts, concurrent POST listeners, and a MCP Python SDK 2.3.0 run with `CHROMIUM_MCP_SDK_PYTHONPATH=/tmp/chromium-mcp-sdk`.
- Additional checks: `gn check out/Default //chrome/browser/agent:agent` passed; the integration covers private-context network events, refusal to close a pre-existing `--incognito` context, SSE disconnect/shutdown, and element accessible-name fixtures.
- External schema spot-check: the `subscriptions/listen`, acknowledgment, and resource-updated message shapes validated against the published MCP Python SDK `schema/2026-07-28.json` downloaded from the official SDK repository.
- Known remaining limitations: role/name matching implements the documented accessible-name sources but does not use Chromium's full AX name computation. Redirected body capture is skipped to preserve Chromium's native redirect body delivery. macOS, Windows, and ChromeOS OS permission verification remains deferred as documented in the canonical TODO.
- Linux GNOME permission evidence: the canonical TODO records reproduced location, camera, and microphone OS-level denials with Chromium site permission granted. Other supported desktop operating systems remain unverified.
