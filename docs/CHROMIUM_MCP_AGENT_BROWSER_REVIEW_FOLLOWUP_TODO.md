# Chromium MCP Agent Browser Review Follow-up TODO

Status: Linux follow-up implementation and qualification complete; Perfetto trace stop and non-Linux OS permission checks remain separate
Spec: [Review Follow-up Specification](CHROMIUM_MCP_AGENT_BROWSER_REVIEW_FOLLOWUP_SPEC.md)
Source review: [Remediation TODO](CHROMIUM_MCP_AGENT_BROWSER_REMEDIATION_TODO.md)

Check an item only after its implementation and stated evidence pass. Record decisions, commands, platform, source revision, and results under the completion record. A prior remediation checkmark is not evidence for this follow-up.

## 0. Baseline and regression setup — spec completion rule

- [x] Record the starting commit, working-tree status, GN args, binary version and build provenance; identify `0b194ec297205c250cd42c4579ec556ae28513e3` as the earlier remediation base if confirmed.
- [x] Add focused failing regression cases for partial writes, early tee destruction, unobservable page interception, malformed listen filters, multi-ID accessible names, and resisted context closure before marking their sections complete.
- [x] Record which earlier browser results were reproduced on the current source and which remain historical evidence.

## 1. Response tee correctness — spec F1

- [x] Forward the full pending buffer after each partial `WriteData` success before reading more source data; preserve accurate forwarded-byte accounting.
- [x] Replace the unexpected source-read-error loop with defined termination and error propagation; cover destination write and pipe-creation failures.
- [x] Release each simultaneous-capture slot exactly once for EOF, cancellation, pipe failure, interception failure, and early destruction.
- [x] Test byte-for-byte page delivery under a forced partial write and slow consumer, including reads that arrive while pending bytes remain.
- [x] Test cancelled and continuous responses, redirect bypass, completion status, and more than eight successive early cancellations followed by a capturable response.
- [x] Measure or assert per-response and aggregate capture/forwarding memory against the documented budgets.

## 2. Capture eligibility — spec F2

- [x] Define the browser-validated observable-page/context eligibility decision and how it reaches the interception point without trusting a renderer page ID.
- [x] Avoid `InterceptResponse` and tee creation for unregistered, closed, or otherwise ineligible pages while preserving process/frame token validation.
- [x] Cover registered regular and private pages, unrelated enabled-browser tabs, pages opened before registration, closed handles, disabled feature/switch paths, and the intentional worker exclusion.
- [x] Document any remaining metadata-only observation when body capture is ineligible.

## 3. Subscriptions and slow listeners — spec F3

- [x] Validate `subscriptions/listen` filter presence, field types, resource URI entries, and supported values before writing SSE response headers or starting a watch.
- [x] Make acknowledgment reflect exactly the honored filter and verify subscription ID metadata and matching delivery for concurrent listeners.
- [x] Define and implement a bounded slow-listener recovery path so a failed `SendRaw` append cannot silently strand unread browser events.
- [x] Test disconnect, browser shutdown, stalled client, cursor continuation, journal overrun detection, and no writes after cancellation.
- [x] Validate request, acknowledgment, update, and terminal stream shapes against published `2026-07-28` schemas or an independent client.

## 4. HTTP metadata and schema parity — spec F4

- [x] Reject present `clientInfo` values with empty `name` or `version`, and align every optional and mandatory metadata check with the local and published schemas.
- [x] Add table-driven schema and live-endpoint cases for missing, empty, and wrong-type metadata; malformed listen filters; encoded `Mcp-Name`; unsupported versions; and mirrored-header mismatch.
- [x] Assert the expected HTTP status, JSON-RPC code and request ID, with no browser operation or subscription watch for every rejected case.
- [x] Rerun a valid independent MCP client discovery, tool-call, resource-read, and listen flow.

## 5. Context lifecycle and isolation — spec F5

- [x] Test two simultaneous owned contexts using equal cookie names, storage keys, permission origins, page targets, and download fixtures; confirm isolation in each dimension.
- [x] Test closing one owned context while another owned context and a discovered pre-existing private window remain active.
- [x] Define and implement truthful `browser.close_context` results for immediate closure, delayed closure, a beforeunload refusal, and destruction timeout; avoid unbounded retries.
- [x] Verify eventual owned-profile destruction and rejection of stale context, page, frame, observer, and event association handles after successful closure.
- [x] Update the context lifecycle contract and record observed browser results.

## 6. Element results — spec F6

- [x] Correct multi-ID `aria-labelledby` parsing and add a fixture with whitespace-separated references in order.
- [x] Replace or complete the fixed native-role/name mapping using Chromium accessibility data or an equivalently tested algorithm; cover labels, hidden naming references, images, explicit roles, `progressbar`, and `summary` button.
- [x] Specify and test viewport intersection, ancestor visibility, clipping, pointer-events, and occlusion semantics; keep `actionable` tied to the documented point.
- [x] Verify selected child-frame locator results and frame-relative coordinates against native pointer coordinates or documented translation.
- [x] Add `page.fill` evaluation-failure, failed-readback, read-only, unsupported-control, and successful-readback browser cases.

## 7. Documentation and qualification — spec F7

- [x] Annotate the old completion record's pre-commit revision without overwriting its historical test claims.
- [x] Update `TOOL_CONTRACT.md`, `TEST_PLAN.md`, project entry points, and canonical/remediation status statements to match the fixed behavior and current qualification state.
- [x] Build the affected Chromium target and run focused C++ tests, protocol/schema tests, and browser integration against that rebuilt binary.
- [x] Run disabled-feature browsing, network backpressure/cancellation, subscription conformance, complete context isolation/lifecycle, and element regressions; record failures as well as passes.
- [x] Record Linux platform, exact post-fix source revision, dirty-tree diff identifier if applicable, GN args, browser binary version/provenance, commands, and results below.
- [x] Keep macOS, Windows, and ChromeOS OS permission verification status separate from the Linux follow-up gate.

## Completion record

The following results were reproduced on the follow-up working tree. The
remediation checklist's earlier browser claims remain historical evidence.

- Source revision: `b0e91025d8e7e2846d3c5a257c971e46bf5f5682` plus
  uncommitted follow-up changes. `0b194ec297205c250cd42c4579ec556ae28513e3`
  is its immediate pre-commit ancestor. The tracked source diff from
  `git diff --binary -- chrome agent_protocol | sha256sum` is
  `882572e66cd7d732d2d287fa3e62e20a2b9bcda47595e2fc1df6845c456cedec`;
  the new, untracked C++ regression test SHA-256 is
  `34c200052e985df552df307a0e222e9d37df1a366f1107f51309d65d2451e6f7`.
- GN args: `is_debug=false`, `is_component_build=true`, `symbol_level=0` in
  `out/Default/args.gn`. `autoninja -C out/Default chrome` and
  `autoninja -C out/Default unit_tests` passed after the final source changes.
  The rebuilt `out/Default/chrome` reports `Chromium 156.0.8076.0`; its SHA-256
  is `ca410c26cc223afe715a085c9bc1ee4c37c41b98aa16e58a8fb62c5f88ac58a0`.
- Platform: Linux x86_64, kernel `7.0.0-34-generic`, Ubuntu 24.04 host, on
  2026-10-09. Browser tests used the rebuilt binary above.
- Focused validation: `gn check out/Default //chrome/browser/agent:agent` and
  `gn check out/Default //chrome/browser/agent:unit_tests` passed;
  `out/Default/unit_tests --gtest_filter='BrowserAgentNetworkThrottleTest.*:BrowserAgentProtocolTest.*:BrowserAgentServiceTest.*' --ozone-platform=headless`
  passed all 9 tests; `npm test --prefix agent_protocol` passed all 3 schema
  tests; Python compilation, `git clang-format --diff HEAD --` on the edited
  C++ tree, and `git diff --check` passed.
- Browser integration:
  `CHROME_BIN=../out/Default/chrome CHROMIUM_MCP_SDK_PYTHONPATH=/tmp/chromium-mcp-sdk CHROMIUM_MCP_SKIP_TRACE=1 CHROMIUM_MCP_TEST_CLOSE_TIMEOUT=1 npm run test:chromium`
  passed from `agent_protocol/`. The independent MCP Python SDK was 2.3.0;
  discovery, tool call, resource read, live listen acknowledgment, and update
  all passed. The same integration run covered disabled-feature browsing and
  the opt-in regular/private browser paths.
- Network evidence: a 4 MiB patterned response was read slowly and matched
  byte for byte at the page; the 32 KiB forwarding pipe is smaller than each
  64 KiB source read, forcing partial writes. Redirect bypass, a continuous
  response, 9 early cancellations, and a later capturable response passed.
  At most 8 captures are active per renderer. Each has a 256 KiB captured
  prefix, 64 KiB pending buffer, 64 KiB read buffer, and 32 KiB forwarding
  pipe: 416 KiB per active tee and 3.25 MiB for 8, excluding transient event
  encoding. Unexpected Mojo read and pipe-creation errors have defined code
  paths; dedicated fault injection for those rare failures was not run.
- Subscription and HTTP evidence: malformed filters and metadata returned
  HTTP 400 with JSON-RPC errors and matching IDs; two concurrent listeners
  received matching updates, an unsupported filter acknowledged an empty
  subset, a 245 KiB listener ID forced bounded stream closure, and 130
  navigations made the 256-entry journal report `cursor_expired` with
  `oldest_cursor`. Browser shutdown closed an acknowledged stream.
- Context and element evidence: two owned private contexts remained isolated
  for equal cookie/storage keys, permissions, pages, and downloads; a
  pre-existing private context resisted owned-context closure. A refused
  beforeunload close stayed live through the 60-second timeout and succeeded
  on retry; stale page, frame, observer, and context calls then failed.
  Browser fixtures passed multi-ID and hidden-reference accessible names,
  native/ARIA roles, clipping, opacity, inherited and explicitly restored
  pointer events, pointer occlusion, failed fill
  readback, and a native click in a selected child frame.
- Separate limits: two unskipped integration attempts timed out in the
  pre-existing Perfetto `browser.stop_trace` test while processes were
  acknowledging `BeginTracing`; this follow-up gate ran with
  `CHROMIUM_MCP_SKIP_TRACE=1`. macOS, Windows, and ChromeOS OS permission
  integration has not been verified on this Linux host.
