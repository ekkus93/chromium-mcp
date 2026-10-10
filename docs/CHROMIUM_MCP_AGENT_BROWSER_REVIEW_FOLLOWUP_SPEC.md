# Chromium MCP Agent Browser Review Follow-up Specification

Status: Linux implementation and qualification recorded in the companion checklist; Perfetto tracing and non-Linux OS permissions are separate
Date: 2026-10-09
Companion checklist: [Review Follow-up TODO](CHROMIUM_MCP_AGENT_BROWSER_REVIEW_FOLLOWUP_TODO.md)
Baseline: [Remediation Specification](CHROMIUM_MCP_AGENT_BROWSER_REMEDIATION_SPEC.md), [Remediation TODO](CHROMIUM_MCP_AGENT_BROWSER_REMEDIATION_TODO.md), and [Tool Contract](TOOL_CONTRACT.md)

## Purpose and completion rule

Address the defects and evidence gaps found while reviewing the Linux remediation implementation at `b0e91025d8e7e2846d3c5a257c971e46bf5f5682`. The earlier remediation checklist is implementation history; its checked boxes do not establish that the follow-up requirements below pass. Keep the browser-owned, opt-in MCP architecture and the `2026-07-28` transport. Preserve Chromium's normal security and permission boundaries.

Each requirement is complete only when its behavior is implemented, a regression test exercises the failure mode, and the result is recorded in the companion checklist. Separate verified behavior from historical test claims and from behavior that cannot be reproduced on the current host.

## F1. Forward response bodies without loss or stalls

**Current path:** `BrowserAgentResponseBodyTee::Pump()` in `chrome/common/agent/browser_agent_network_throttle.cc` writes `pending_body_` with `MOJO_WRITE_DATA_FLAG_NONE`. A successful Mojo write can be partial. The loop then reads another source chunk and replaces `pending_body_` before forwarding its remaining bytes. Other source read errors loop without closing the source. Capture slots are released only in the normal completion callback.

**Required behavior**

- Drain all pending bytes before reading another source chunk. Treat a successful partial write as outstanding work; arm the producer watcher when appropriate. Count forwarded bytes only after a successful write.
- On source read or destination write failure, terminate or transition to a defined forwarding state without a busy loop. Preserve the underlying completion status and close the page-facing pipe in a way that lets Chromium observe the failure.
- Make the global capture slot an owned resource released exactly once on normal EOF, cancellation, interception failure, pipe error, and destruction before the completion callback. Do not rely on a callback that teardown can bypass.
- Keep the per-response capture limit, simultaneous capture limit, and forwarding capacity bounded. A capture failure must never cause an incomplete body to be reported as complete.

**Acceptance:** a test forces a successful partial write followed by additional readable source data and compares every page byte with the fixture. Slow-consumer, cancellation, premature destruction, pipe-error, continuous-response, and redirect cases terminate without a spin, preserve expected completion/error behavior, and leave all capture slots available for later responses. Run a browser fixture with more than eight successive early cancellations, then confirm a later response is captured.

## F2. Decide capture eligibility before interception

**Current path:** `URLLoaderThrottleProviderImpl::CreateThrottles()` creates an agent throttle for every eligible frame subresource when the feature and switch are enabled. `BrowserAgentNetworkEventHost` later drops reports when no registered page can be found. That late check does not prevent the response tee from intercepting unrelated page traffic.

**Required behavior**

- Establish whether a request belongs to a currently observable page and live context before calling `InterceptResponse` or creating the tee. Define how that eligibility reaches the renderer throttle or browser-owned interception decision without trusting a renderer-supplied page ID.
- Preserve the existing process/frame token validation at the Mojo boundary. Recheck eligibility at the point of interception when a page or context may have closed since request start.
- Cover regular and off-the-record pages, an enabled MCP service with an unregistered tab, a closed handle, and pages opened before registration. Keep worker traffic explicitly excluded unless it gains a validated page association.
- When capture is ineligible, allow Chromium's original response path to proceed without an agent tee. Document whether lightweight request metadata is still observed and why.

**Acceptance:** instrumentation or a focused test proves that unrelated frame subresources are not intercepted or teed, while registered regular and private pages still produce correctly associated events. The feature-off and switch-off paths remain free of agent throttles.

## F3. Validate and deliver subscriptions consistently

**Authority:** the published MCP [`2026-07-28` Streamable HTTP transport](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http), [subscriptions pattern](https://modelcontextprotocol.io/specification/2026-07-28/basic/patterns/subscriptions), and published revision schema.

**Current path:** `BrowserAgentMcpServer::OnHttpRequest()` accepts `subscriptions/listen` when `params.notifications` is absent or malformed. It opens an SSE stream and acknowledges an empty filter, although the local request schema requires a typed `notifications` object. `HttpServer::SendRaw()` uses a bounded per-connection write buffer, but a failed append is not surfaced to the subscription owner; a slow listener may silently miss an update that should prompt a cursor read.

**Required behavior**

- Validate the entire listen filter before writing SSE headers. Require the fields and types mandated by the published schema, reject invalid resource subscription entries, and define behavior for unsupported resource URIs and unsupported but well-formed notification kinds. The acknowledged subset must exactly match the filter the server will honor.
- Keep subscription ID metadata on every subscription notification and send only events authorized by the acknowledged filter. A disconnect must remove the watch before further writes.
- Define a bounded slow-listener policy. Do not silently lose the sole `notifications/resources/updated` signal for unread browser events. If the send queue cannot accept a notification, close the stream in a detectable way or provide an equivalent explicit recovery signal; document how the client resumes with the resource cursor and detects journal overrun.
- Keep concurrent listener and browser shutdown behavior conforming to the revision. Do not move request-scoped notifications onto a listen stream.

**Acceptance:** negative HTTP cases for missing, wrong-type, and invalid filter fields fail before a watch starts. Independent or published schema validation accepts valid request, acknowledgment, update, and shutdown shapes. Concurrent listeners receive only subscribed updates. A deliberately stalled listener does not cause unbounded memory growth or silent, indefinite loss of browser event notification.

## F4. Keep HTTP metadata validation and the local schema aligned

**Current path:** the schema requires nonempty optional `clientInfo.name` and `clientInfo.version`; the server checks only that those fields are strings. The schema and server also differ on malformed listen filters.

**Required behavior**

- Apply the published revision's required and optional metadata rules at runtime. If `clientInfo` is present, validate nonempty `name` and `version`, matching the local schema. Keep mandatory protocol version and client capabilities validation, header/body comparison, Base64-sentinel `Mcp-Name` decoding, and the published HTTP/JSON-RPC error mapping.
- Compare local schema acceptance with endpoint behavior for missing fields, empty strings, wrong types, encoded names, unsupported versions, and listen filters. For cases where the transport permits a request that a specific method rejects, record the layer and expected error.
- Confirm invalid input causes no browser operation and does not create a subscription watch.

**Acceptance:** table-driven protocol tests show the same accept/reject decision at the schema and endpoint for every applicable request shape, with the specified HTTP status, JSON-RPC code, and request ID. Valid independent MCP client requests still succeed.

## F5. Finish context ownership and lifecycle qualification

**Current path:** `browser.create_context` uses unique DevTools off-the-record profiles, and `browser.close_context` removes handles before requesting window close, then retries profile destruction while windows remain. Existing tests prove distinct IDs and some cookie and permission isolation; they do not establish the full storage, download, handle, and resisted-close claims in the remediation checklist.

**Required behavior**

- Verify isolation between two live created contexts for cookies, local and session storage, site permissions, pages, and download managers using the same origins and names where relevant. Verify that a discovered pre-existing private profile is never treated as owned.
- Define `browser.close_context` success precisely when a window refuses or delays closure, including a beforeunload prompt. Do not report `closed: true` while a window remains open without documenting an asynchronous close contract and observable terminal state. Avoid an unbounded retry loop.
- Verify page, frame, observer, context, and event association handles become unusable after successful closure, while another created context and an existing private window remain live. Verify eventual profile destruction or report a concrete timeout/failure.

**Acceptance:** browser scenarios exercise the complete isolation matrix, close-one-keeps-other behavior, pre-existing incognito protection, stale handles, and refused or delayed window closure. Record the observed lifecycle and error/result semantics in `TOOL_CONTRACT.md`.

## F6. Make element queries match accessible state

**Current path:** `page.find_elements` builds a small native-role mapping in injected JavaScript and splits `aria-labelledby` with `/\\s+/` inside a C++ raw string. That JavaScript regular expression does not split whitespace. The existing fixture has only one referenced ID. Visibility and `actionable` are based on the element bounds and one center-point hit test.

**Required behavior**

- Fix multi-ID `aria-labelledby` in the immediate implementation, preserving ID order and normal whitespace separation. Cover hidden but name-contributing references and fallback rules where applicable.
- Obtain roles and accessible names from Chromium accessibility data, or implement and test an equivalent set of native and ARIA rules without a fixed native-element selector subset. Include at least labels, `aria-label`, multiple `aria-labelledby` references, image alternatives, native roles such as `progressbar` and `summary` button, and explicit roles. Keep matching and case rules documented.
- Define the limits of `visible` and `actionable`: viewport intersection, ancestor visibility/opacity, clipping, pointer-events, and occlusion outside the center point. Either make the result reflect interactability for the reported point or document and name the narrower center-point test precisely. Keep selected child-frame coordinates explicitly frame-relative.
- Keep `page.fill` success tied to a live supported target and value readback. Add evaluation-failure and failed-readback fixtures alongside read-only and unsupported controls.

**Acceptance:** browser fixtures prove the named accessible-role and name cases, hidden/offscreen/clipped/obscured cases, child-frame locators and coordinates, and all fill failure and success paths. A query for a valid multi-ID accessible name returns the intended element; unsupported native roles are no longer silently omitted.

## F7. Make qualification and documentation reproducible

**Current path:** the remediation completion record names the parent commit `0b194ec297205c250cd42c4579ec556ae28513e3` as its source revision and describes remediation as uncommitted working-tree changes. The reviewed implementation is committed at `b0e91025d8e7e2846d3c5a257c971e46bf5f5682`.

**Required behavior**

- Preserve the historical completion record, but annotate its revision as the pre-commit base if that is what it represents. Record the exact post-fix source commit, any dirty-tree diff identifier, GN args, platform, binary version, build command, and binary provenance used for follow-up validation.
- Update `TEST_PLAN.md`, `TOOL_CONTRACT.md`, project entry points, and both remediation status summaries after the implementation and qualification pass. Do not change a historical test claim into a current result without rerunning it.
- Run focused C++ tests, schema/protocol tests, browser integration against the rebuilt binary, an independently maintained MCP client or published conformance fixtures, disabled-feature browsing, large/partial-write/cancellation network fixtures, context isolation and resisted closure, and the element fixtures above. Record commands and outcomes. Keep macOS, Windows, and ChromeOS permission verification explicitly separate from Linux qualification.

**Acceptance:** the follow-up checklist contains a reproducible source-to-binary chain and pass/fail evidence for every section. Current entry points distinguish implemented behavior, verified behavior, and remaining platform limitations without conflicting completion claims.
