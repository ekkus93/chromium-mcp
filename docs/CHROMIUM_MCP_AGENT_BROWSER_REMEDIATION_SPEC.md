# Chromium MCP Agent Browser Remediation Specification

Status: Implemented and validated on Linux; see the remediation checklist for residual limitations
Date: 2026-10-09
Companion checklist: [Remediation TODO](CHROMIUM_MCP_AGENT_BROWSER_REMEDIATION_TODO.md)
Baseline: [Agent Browser Specification](CHROMIUM_MCP_AGENT_BROWSER_SPEC.md) and [canonical TODO](CHROMIUM_MCP_AGENT_BROWSER_TODO.md)

## Purpose and completion rule

Resolve the six findings from the 2026-10-09 code review: network capture while disabled and unbounded buffering; MCP transport and validation mismatches; reused private contexts; inaccurate element results; and stale documentation. Keep browser control in Chromium through native APIs, preserve loopback authentication and normal Chromium security boundaries, and avoid CDP or a sidecar.

The original TODO records implementation history. Its checked boxes do not serve as proof of this remediation. Completion requires the behavior and evidence below. Update the original TODO's status and validation notes after the work passes, with links to this specification and the remediation checklist.

## R1. Make network observation opt-in and bounded

**Current path:** `chrome/renderer/url_loader_throttle_provider_impl.cc` installs `BrowserAgentNetworkThrottle` for frame subresources, and `chrome/common/agent/browser_agent_network_throttle.cc` tees each response into an unbounded `std::string`. The browser host later drops events if no active service exists. This still intercepts and buffers page traffic.

**Required behavior**

- With the feature or runtime switch disabled, no agent throttle or response-body tee is installed. Do not rely on the browser host dropping events after interception. Verify behavior for regular and off-the-record renderers and for workers if they use the same provider.
- With MCP enabled, only traffic belonging to an observable page/context enters the capture path. Preserve process/frame validation at the browser-owned Mojo boundary.
- Bound memory per response and across simultaneous responses. Define a capture budget before implementation. Preserve the original response stream, completion status, and error propagation under slow consumers, cancellation, redirects, pipe failures, and large or continuous bodies.
- A body event states the captured byte count, total size when known, and whether it is truncated. If full-body access remains part of the tool contract, provide a bounded retrieval mechanism; otherwise document the finite capture limit and change the contract accordingly. A limit is a resource bound, not an agent permission tier.
- Avoid retaining duplicate full-body copies during Base64 encoding or event queueing. Bound or drain queued network events so an authenticated client that does not poll cannot cause unbounded growth.

**Acceptance:** disabled browsing has no agent interception; enabled capture returns correct bytes and truncation metadata for small and oversized fixtures; page downloads complete with unchanged bytes under backpressure; peak retained capture memory stays within the documented budget.

## R2. Implement MCP Streamable HTTP revision 2026-07-28

**Authority:** the published [Streamable HTTP transport](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http), [subscriptions pattern](https://modelcontextprotocol.io/specification/2026-07-28/basic/patterns/subscriptions), [versioning rules](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning), and the revision's server feature schemas. Keep version-specific behavior explicit if older clients are deliberately supported.

**Required behavior**

- Accept one JSON-RPC request per POST. Remove the standalone `GET /mcp` event stream for requests claiming `2026-07-28`; respond to GET with `405 Method Not Allowed` for this revision.
- Deliver long-lived change notifications on the response stream of `subscriptions/listen` POST, with the subscription request, notification types, subscription ID metadata, and lifecycle defined by the published revision. Keep request-scoped notifications on their originating POST response stream. Closing an SSE response cancels its request, stops its watch promptly, and prevents later writes to that stream.
- Preserve browser tab, popup, and navigation events through a documented subscription design. The core `subscriptions/listen` filter covers list changes and resource updates; it does not authorize arbitrary `notifications/message` events on the listen stream. Use a subscribable browser-events resource with `notifications/resources/updated` and a bounded event read/cursor API, or define and negotiate a custom extension for direct event notifications. Select one design in the implementation record and test its filter, acknowledgment, subscription ID, and event delivery. Do not invent a connection-scoped MCP session for this revision.
- Return the specified HTTP status and JSON-RPC error shape for unsupported methods, malformed requests, and unsupported protocol versions. Unsupported protocol versions include the supported version list. Keep ordinary tool execution errors in the tool result where the protocol requires it.
- Update discovery capabilities, local schema, client fixtures, tool contract, setup examples, and integration tests to use the revised transport. Do not treat a client and schema authored in this repository as independent compatibility evidence; include a conformance check against published schemas or an independently maintained MCP client.

**Acceptance:** a conforming `2026-07-28` client can discover, list, call, and listen over POST; GET does not open a stream; disconnect stops a listen watch; unsupported methods and versions have the specified status and error payload.

## R3. Validate HTTP metadata and headers exactly

**Current path:** `BrowserAgentMcpServer::OnHttpRequest` requires some mirrored headers but accepts absent body metadata, compares `Mcp-Name` literally, and sends plain JSON objects for header errors.

**Required behavior**

- Require the revision's mandatory per-request `_meta` fields: protocol version and client capabilities. Validate their types, validate optional client identity when present, and compare the body version with `MCP-Protocol-Version`. Missing mandatory fields return HTTP 400 with JSON-RPC `-32602`.
- Validate `Mcp-Method` and applicable `Mcp-Name` against the parsed body. Decode the revision's Base64 sentinel form before comparing `Mcp-Name`, including URI values with non-ASCII or header-unsafe bytes. Reject malformed encoding and invalid header values.
- For missing or mismatched required mirrored headers, return HTTP 400 with JSON-RPC error `-32020` (`HeaderMismatch`) and the appropriate request ID. For an unsupported version, return the revision's `UnsupportedProtocolVersionError` with supported versions. Use the published rule for unknown methods.
- Validate `Content-Type`, JSON-RPC request shape, and notification handling under the revision. Preserve loopback peer, exact Host, Origin, and bearer-token checks before tool dispatch. Avoid recording sensitive arguments or tokens in audit output.
- Make the local schema and runtime enforce the same requirements. Add negative cases for omitted metadata, mismatched headers, encoded names, unsupported versions, and malformed JSON.

**Acceptance:** each valid request reaches its intended method; each invalid request is rejected with the specified HTTP status and JSON-RPC body before any browser action occurs.

## R4. Give created contexts independent ownership

**Current path:** `browser.create_context` calls `GetPrimaryOTRProfile(true)`, so repeated calls return the same profile and share cookies, storage, permissions, and windows. `browser.close_context` can close a pre-existing private session.

**Required behavior**

- Define the supported `browser.create_context` contract as creation of a distinct private context on every successful call. Two live created handles must have separate storage, permission state, pages, and download managers. A created context must never alias Chromium's existing primary private profile.
- Before implementation, verify which Chromium profile/container mechanism can both create independent ephemeral storage and host browser windows. `Profile::OTRProfileID::CreateUnique` alone is insufficient: non-primary IDs generally cannot open browser windows. Choose a Chromium-supported design and record its lifecycle implications. Do not label an attached primary private profile as newly created.
- Track whether a context is owned by this MCP service or merely discovered. Closing a created context closes only its own windows and destroys its own profile after dependent objects release it. Closing or observing a pre-existing private context must not destroy unrelated user windows. Invalidate its page, frame, observer, and context handles together.
- Make creation failure explicit when Chromium cannot provide the stated isolation. Do not return a successful handle that aliases another context. Ensure context listing, page creation, and network observation use the same ownership mapping.

**Acceptance:** create two contexts concurrently, write the same cookie and storage key with different values, and confirm isolation; close one and confirm the other and any pre-existing private window remain live; stale handles fail after closure.

## R5. Make element results accurate

**Current path:** `page.fill` marks success even when script evaluation returns no result. `page.find_elements` uses a small handwritten role/name mapping and `getClientRects()` as its visibility test.

**Required behavior**

- `page.fill` must treat a missing result, JavaScript exception, unsupported or read-only target, and a failed value readback as an error. Report `filled: true` only after the requested value is observed on the intended live element. Keep the documented DOM-event behavior clear.
- Implement role and accessible-name queries from Chromium's accessibility data or an equivalent implementation of the browser's accessible-name rules. Handle at least `aria-labelledby`, explicit labels, `aria-label`, image alternatives, and native roles without a fixed selector subset. Define substring/exact matching and case behavior in the tool contract.
- Define "visible" in terms usable for interaction: exclude hidden, zero-area, and fully outside-viewport matches; identify occlusion or return an explicit `actionable` field rather than calling an obscured element visible. Return frame-relative or viewport-relative coordinates with the coordinate space documented.
- Keep CSS selector and text queries working within selected child frames. Avoid claiming that a main-frame coordinate can click an element in an out-of-process frame without translation.

**Acceptance:** fixtures cover labeled controls, `aria-labelledby`, hidden/offscreen/obscured elements, child frames, read-only or unsupported controls, evaluation failure, and successful value readback. No failed fill reports success.

## R6. Reconcile project documentation

- Update `PROJECT_ORIENTATION.md` and `TOOL_CONTRACT.md` to match the implemented transport, context semantics, capture limits, and current feature status. Include the renderer provider, browser receiver binding, and `chrome/common/agent/` files among rebase conflict points.
- Mark `CHROMIUM_MCP_AGENT_BROWSER_HANDOFF.md` as a historical snapshot or replace its obsolete build and validation statements; keep the dated 2026-10-09 handoff as historical evidence.
- Update `TEST_PLAN.md`, endpoint examples, and the canonical TODO after implementation and validation. State which OS permission results were reproduced and which remain documented evidence.
- Keep source placement guidance accurate: most service code is under `chrome/browser/agent/`, while the network hook necessarily crosses `chrome/renderer/` and `chrome/common/agent/`.

**Acceptance:** a contributor following the current project entry points sees one consistent protocol revision, event-stream setup, context behavior, test command set, and upstream conflict list.

## Validation and release gate

Build the affected Chromium targets, run focused C++ tests, protocol/schema tests, and the browser integration suite with the rebuilt Chrome binary. Add the specific regression cases above. Check the disabled-feature path in a normal browser run, and inspect memory behavior for an oversized or continuous response. Record command, binary revision, platform, and result in the remediation TODO. Do not mark the remediation complete solely from passing the existing fixtures.
