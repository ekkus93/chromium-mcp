# Chromium MCP Agent Browser Tool Contract

Status: Remediation in progress; see the linked current checklist
Companion spec: `docs/CHROMIUM_MCP_AGENT_BROWSER_SPEC.md`
Canonical TODO: `docs/CHROMIUM_MCP_AGENT_BROWSER_TODO.md`
Current remediation: `docs/CHROMIUM_MCP_AGENT_BROWSER_REMEDIATION_TODO.md`

## Contract purpose

Chromium itself hosts MCP and calls its browser APIs directly. The contract follows the current MCP Streamable HTTP specification; it does not define a sidecar IPC protocol or a CDP adapter.

## Layering

```text
MCP client -- Streamable HTTP on loopback --> Chromium BrowserAgentService
                                              |
                                              +-- native Browser/Profile/WebContents APIs
```

The browser process receives MCP JSON-RPC requests, validates protocol syntax and tool arguments, then dispatches to native Chromium APIs. Validation handles malformed input and object lifetime errors; it must not impose agent-specific permission tiers or approval gates.

## MCP methods

Implement the MCP server discovery and tool methods, including `server/discover`, `tools/list`, and `tools/call`. Current tools are `browser.version`, `browser.list_contexts`, `browser.create_context`, `browser.close_context`, `context.storage_state` (native cookie read), `context.set_cookies` (native cookie write), `page.list`, `page.new`, `page.get_info`, `page.frames`, `page.screenshot` (native surface capture returned as MCP image content), `page.accessibility_snapshot` (native accessibility node tree), `page.wait_for_load_state` (load completion), `page.wait_for_url` (exact URL commit), `page.evaluate` (page main world, optionally a selected frame), `page.click` and `page.hover` (native mouse events at viewport coordinates), `page.check` and `page.uncheck` (native checkbox clicks), `page.type_text` (native keyboard character events), `page.press` (common named native key events), `page.fill` (CSS-selected editable elements), `page.focus_element` (CSS-selected focus), `page.select_option` (select by value), `page.goto`, `page.back`, `page.forward`, `page.reload`, `page.stop`, `page.close`, and `page.focus`; the full planned surface is in the TODO.

Every MCP operation uses one JSON-RPC request per POST. A
`subscriptions/listen` POST opens an SSE response for the requested
`chromium-mcp://browser/events` resource. The server acknowledges the honored
filter and sends `notifications/resources/updated` with the subscription ID
when browser events change. Read the resource with
`chromium-mcp://browser/events?after=<cursor>` to fetch event records; the
journal retains the newest 256 records. Closing the SSE response stops its
watch. GET is rejected with HTTP 405 for protocol revision `2026-07-28`.

`page.visible_content` returns the main frame's visible text, headings, and
links. It complements the cross-frame native accessibility snapshot.
`page.console_messages` starts a native `WebContentsObserver` on first call and
returns messages collected since the previous call, then clears that buffer.
`page.network_events` uses the same observer to return completed resource loads
since the previous call, including original and final URL, method, HTTP status,
network error, MIME type, cache flag, body byte count, referrer, request ID,
request destination, total received bytes, and whether Chromium accessed the
network. Main-frame navigation lifecycle events also include native request
and response headers. A native URL loader throttle also records subresource
request and response headers, including CORS-exempt request headers, and
captures response bodies as Base64 bytes while teeing the original stream to
the page. Each captured body is limited to 256 KiB; `captured_bytes`,
`total_size`, `total_size_known`, and `truncated` describe the capture. At most
8 response bodies are captured at once across a renderer; additional bodies
are forwarded without capture and report `capture_skipped`. Each tee reads
through a 64 KiB pending buffer and a 1 MiB bounded forwarding pipe, so a slow
page consumer eventually backpressures the network pipe. At most 8 MiB of
forwarding-pipe capacity and 2 MiB of captured body strings are active per
renderer at once. A body event reports `forwarded_bytes`, the bytes accepted by
the page-facing pipe. Network event queues are limited to 16 MiB and 1024
events process-wide, with at most 16 queued events per page; older events are
dropped when a limit is reached. Body events also report `capture_error` and
the low-level `capture_error_result` when a pipe operation fails. A generated request ID links the request,
response, and body events. Full-body retrieval is not provided; the body event
contains only the finite captured prefix.
Redirected subresource bodies report `capture_skipped` with reason
`redirected_response`; the redirect-following loader forwards them without
installing the body tee.
The resource-completion event separately reports the final byte counts.
Renderer interception requires both `BrowserAgentService` and the
`--enable-browser-agent-service` switch and applies only to non-document
requests from frame throttle providers with a local frame token. Worker
throttle providers do not enter this page-associated capture path.
`context.clear_http_cache` clears the selected context's native HTTP cache.
`context.set_accept_language`, `context.set_referrers_enabled`, and
`context.close_idle_connections` expose the corresponding native
`NetworkContext` controls. `context.set_accept_language` also updates the
context's native accepted-language preference and renderer settings.
`resources/list` exposes `chromium-mcp://browser/version`,
`chromium-mcp://browser/contexts`, and the subscribable
`chromium-mcp://browser/events` resource. `resources/read` returns their JSON content
with private, immediately stale cache hints. Unknown resource URIs return
JSON-RPC `-32602`.
`page.accept_dialog` and `page.dismiss_dialog` act on a page's active
JavaScript dialog through Chromium's dialog manager. The accept tool accepts
an optional `prompt_text` override. `page.dialog_info` reads the active
tab-modal alert, confirm, or prompt type, title, message, and default prompt;
it also reports the active app-modal beforeunload dialog for that page.
`context.downloads` lists native download items for a regular or private
context, including their GUID, URL, state, byte counts, and target path.
`page.download` starts a URL download associated with the page's main frame;
an optional `path` selects the target file. The response includes the native
download GUID, which can be found in `context.downloads`.
`page.prepare_file_upload` supplies absolute local paths for the next file
input chooser in the page's main frame. Follow it with a native `page.click` on
the file input; Chromium's chooser completion grants the renderer access to
the selected files. The selection is consumed once.
`download.cancel` cancels an item by GUID in the selected context.
`download.wait` observes that item until it completes, is cancelled, is
interrupted, or reaches the 30-second timeout.
`context.permission` and `context.set_permission` read or change Chromium
content settings. Common aliases include geolocation, notifications, camera,
microphone, and popups; every registered setting name is also accepted.
`context.list_permissions` discovers the names and valid values for this
Chromium build. `page.permission_status` reports both the browser permission
and combined OS/device status for geolocation, camera, microphone, and
notifications on the current document. OS and browser permission boundaries
still apply.
`page.find_elements` returns viewport-intersecting matches for a CSS selector,
role, name, and/or text query. `actionable` indicates whether the element is
unobscured at its center. Coordinates are relative to the selected frame's
viewport; child-frame coordinates need translation before using main-page
native pointer tools. Name and text filters use case-sensitive substring
matching.
`page.set_user_agent` uses `WebContents::SetUserAgentOverride` for a page,
marks its current navigation entry, and clears the override when passed an
empty string. Reload the page to apply the change to its current document.
`page.set_viewport_size` resizes the page through `WebContents::Resize`; it
changes the viewport without emulating a mobile device or changing device
pixel ratio.
`page.accessibility_snapshot` includes native roles, names, descriptions,
values, placeholders, hierarchy, expansion/required/busy/modal states, and
available restriction, checked, heading, and set-position attributes.
`page.storage_state` reads local and session storage for the page frame's
origin. `page.set_storage` sets string entries in either area through direct
page evaluation. Both accept a `frame_id` from `page.frames`, allowing the
client to import or export storage for an open cross-origin frame using direct
frame execution under Chromium's same-origin rules.
`context.local_storage_origins` lists all native local storage keys in a
context with their origins and sizes, including origins without an open page.
`context.clear_local_storage` deletes a listed native storage key,
including its partition identity, without opening that origin in a page.
Closing an in-flight HTTP request cancels its pending response. Native
navigation and load waiters also detach immediately; effects already handed to
Chromium, such as a navigation or cookie write, cannot be rolled back.
With `--browser-agent-audit-log=<path>`, accepted MCP requests are appended as
JSON Lines on a background task runner. Each entry records time, method, and
tool name; request arguments and bearer tokens are not written.
With `--browser-agent-trace-dir=<directory>`, `browser.start_trace` starts
Chromium tracing for the requested categories and `browser.stop_trace` writes
the completed JSON trace to a uniquely named file in that directory.

For revision `2026-07-28`, send one JSON-RPC request per POST with
`Content-Type: application/json`, `Accept: application/json, text/event-stream`,
`MCP-Protocol-Version`, and `Mcp-Method`. Every request includes
`params._meta.io.modelcontextprotocol/protocolVersion` and
`params._meta.io.modelcontextprotocol/clientCapabilities`. Calls to
`tools/call` and `resources/read` also mirror the tool name or URI in
`Mcp-Name`; unsafe values use the published Base64 sentinel encoding. GET
returns HTTP 405. A browser event subscription uses this POST body:

```json
{
  "jsonrpc": "2.0",
  "id": "events-1",
  "method": "subscriptions/listen",
  "params": {
    "notifications": {
      "resourceSubscriptions": ["chromium-mcp://browser/events"]
    },
    "_meta": {
      "io.modelcontextprotocol/protocolVersion": "2026-07-28",
      "io.modelcontextprotocol/clientCapabilities": {}
    }
  }
}
```

Use MCP's standard JSON-RPC envelopes. Example tool call:

```json
{
  "jsonrpc": "2.0",
  "id": 7,
  "method": "tools/call",
  "params": {
    "name": "page.evaluate",
    "arguments": { "page_id": "page_...", "expression": "document.title" },
    "_meta": {
      "io.modelcontextprotocol/protocolVersion": "2026-07-28",
      "io.modelcontextprotocol/clientCapabilities": {}
    }
  }
}
```

Return standard MCP tool results with structured content where appropriate. Tool schemas document Chromium inputs and outputs; they do not hide sensitive values or disallow native browser operations.
Malformed JSON-RPC requests return standard `-32700`, `-32600`, `-32601`, or
`-32602` errors. A tool that runs but cannot complete returns `isError: true`
with `structuredContent.error` and a stable `error_code`: `invalid_argument`,
`not_found`, `timeout`, or `browser_error`.

## Native control requirements

- Use browser-process APIs and existing Chromium interfaces directly.
- Evaluate JavaScript in the page main world through the browser-process `RenderFrameHost` API.
- Do not use CDP, remote debugging, DevTools protocol, Playwright, or a separate sidecar as the control path.
- Expose direct evaluation, page data, browser permissions, file selection, network, and storage capabilities to the authenticated local client.
- Let normal Chromium/OS permission behavior apply; do not add agent-specific policy or confirmation layers.
- Keep optional audit/trace output observational and non-blocking.

## Endpoint and runtime

The MCP endpoint is a loopback Streamable HTTP service owned by `BrowserAgentService`. It starts only when the feature and explicit runtime switch are enabled. Validate HTTP origins and authenticate local clients so web pages cannot invoke the endpoint. Endpoint authentication selects who may control the browser; it must not reduce that client's tool capabilities.

The service starts the endpoint when `--browser-agent-mcp-port=<port>` is supplied; use port `0` to request an available loopback port. It writes the selected endpoint and per-profile bearer token under `<profile>/BrowserAgentRuntime/`. See `agent_protocol/` for schema tests and the official [MCP specification](https://modelcontextprotocol.io/specification/2026-07-28).
