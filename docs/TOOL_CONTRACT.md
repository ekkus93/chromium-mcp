# Chromium MCP Agent Browser Tool Contract

**Status:** Linux implementation and review follow-up qualification complete; Perfetto trace-stop work and non-Linux OS permission checks remain separate. See the [current checklist](CHROMIUM_MCP_AGENT_BROWSER_REVIEW_FOLLOWUP_TODO.md).

**Related documents:** [Agent Browser specification](CHROMIUM_MCP_AGENT_BROWSER_SPEC.md) · [Canonical TODO](CHROMIUM_MCP_AGENT_BROWSER_TODO.md) · [Remediation specification](CHROMIUM_MCP_AGENT_BROWSER_REMEDIATION_SPEC.md) · [Development guide](CHROMIUM_MCP_DEVELOPMENT.md)

**On this page:** [Overview](#overview) · [Tool catalog](#tool-catalog) · [Behavior details](#tool-behavior-details) · [HTTP and JSON-RPC](#http-and-json-rpc-contract) · [Runtime and security](#runtime-and-security)

## Overview

Chromium hosts the MCP server in the browser process and calls its native browser APIs directly. Requests use Streamable HTTP over a loopback endpoint. The server validates MCP requests and tool arguments, then dispatches to Chromium APIs; it does not use CDP, Playwright, or a separate sidecar for browser control. For Codex and Claude Code AppImage setup, a small stdio bridge starts Chromium and forwards MCP messages to that loopback endpoint; all browser operations still run in Chromium.

```text
Codex / Claude Code -- stdio --> Chromium MCP bridge
                                      |
                                      +-- Streamable HTTP on loopback --> Chromium BrowserAgentService
                                                                           |
                                                                           +-- native Browser/Profile/WebContents APIs

Other MCP clients ------------------ Streamable HTTP on loopback ---------> Chromium BrowserAgentService
```

The tables below list the tools currently registered by `tools/list`. The two tracing tools appear only when `--browser-agent-trace-dir=<directory>` is configured. Tool schemas in the server define each tool's arguments.

MCP methods include `server/discover` for server metadata, `tools/list` for this catalog, and `tools/call` to invoke a tool. `resources/list` and `resources/read` expose browser resources; `subscriptions/listen` streams browser-event updates.

## Tool catalog

### Browser

| Tool | What it does |
| --- | --- |
| `browser.version` | Returns Chromium version metadata. |
| `browser.list_contexts` | Lists the regular context and existing private contexts. |
| `browser.create_context` | Creates an isolated private browser context. |
| `browser.close_context` | Closes a private context by its `context_id`. |
| `browser.start_trace`* | Starts Chromium tracing for selected categories. |
| `browser.stop_trace`* | Stops tracing and writes the trace as a JSON file. |

\* Available only when tracing output is configured.

### Contexts, storage, network, and permissions

| Tool | What it does |
| --- | --- |
| `context.storage_state` | Reads cookies for a browser context. |
| `context.set_cookies` | Writes cookies through Chromium's native cookie manager. |
| `context.clear_http_cache` | Clears the context's native HTTP cache. |
| `context.local_storage_origins` | Lists local storage keys, origins, and sizes in a context. |
| `context.clear_local_storage` | Deletes local storage for a selected native storage key. |
| `context.set_accept_language` | Sets the context's native `Accept-Language` preference and request header. |
| `context.set_referrers_enabled` | Enables or disables referrer headers for the context. |
| `context.close_idle_connections` | Closes idle native network connections in the context. |
| `context.permission` | Reads a site's native content permission setting. |
| `context.set_permission` | Changes a site's native content permission setting. |
| `context.list_permissions` | Lists content setting names registered in this Chromium build. |
| `context.downloads` | Lists native downloads associated with a context. |

### Downloads

| Tool | What it does |
| --- | --- |
| `download.cancel` | Cancels a download by GUID in a context. |
| `download.wait` | Waits for a download to complete, cancel, interrupt, or time out. |

### Pages and navigation

| Tool | What it does |
| --- | --- |
| `page.list` | Lists open pages in a context. |
| `page.new` | Opens a new page in a context at the requested URL. |
| `page.get_info` | Returns a page's URL, title, and loading state. |
| `page.frames` | Lists the page's current frames. |
| `page.goto` | Navigates a page to a URL. |
| `page.back` | Navigates back in page history. |
| `page.forward` | Navigates forward in page history. |
| `page.reload` | Reloads the current page. |
| `page.stop` | Stops the current page load. |
| `page.close` | Closes a page. |
| `page.focus` | Activates a page and its window. |
| `page.wait_for_load_state` | Waits for a page to reach a requested load state (30-second timeout). |
| `page.wait_for_url` | Waits for an exact URL navigation commit (30-second timeout). |
| `page.set_viewport_size` | Resizes the page viewport through Chromium's native `WebContents` API, without mobile emulation. |
| `page.set_user_agent` | Sets or clears the page's native user-agent override. |

### Inspecting pages

| Tool | What it does |
| --- | --- |
| `page.screenshot` | Captures the page and returns a PNG image. |
| `page.accessibility_snapshot` | Returns the page's native accessibility node tree. |
| `page.visible_content` | Returns visible text, headings, and links from the main frame. |
| `page.find_elements` | Finds visible, viewport-intersecting elements by CSS selector, role, name, or text; reports whether each element's center is unobscured. |
| `page.evaluate` | Evaluates JavaScript in the page's main world, or in a selected frame. |
| `page.console_messages` | Returns and clears collected console messages for the page. |
| `page.network_events` | Returns and clears completed resource-load events for the page. |
| `page.permission_status` | Reports browser and OS/device status for a supported page permission. |
| `page.dialog_info` | Describes the page's active JavaScript or beforeunload dialog. |

### Interacting with pages

| Tool | What it does |
| --- | --- |
| `page.click` | Sends a native left click at viewport coordinates. |
| `page.hover` | Sends a native mouse move to viewport coordinates. |
| `page.fill` | Sets a supported input, textarea, or contenteditable value and dispatches bubbling `input` and `change` events. |
| `page.focus_element` | Focuses an element selected by CSS. |
| `page.select_option` | Selects an option by value in a CSS-selected `<select>`. |
| `page.check` | Checks a CSS-selected checkbox using native input. |
| `page.uncheck` | Unchecks a CSS-selected checkbox using native input. |
| `page.type_text` | Sends text through Chromium's native keyboard input path. |
| `page.press` | Presses a supported named key through native input. |
| `page.accept_dialog` | Accepts the active JavaScript dialog or prompt; can supply prompt text. |
| `page.dismiss_dialog` | Dismisses the active JavaScript dialog or prompt. |
| `page.prepare_file_upload` | Supplies local paths to the next native file chooser for the page. |

### Page storage and downloads

| Tool | What it does |
| --- | --- |
| `page.storage_state` | Reads local or session storage for a page frame's origin. |
| `page.set_storage` | Sets string entries in local or session storage for a page frame's origin. |
| `page.download` | Starts a URL download in the page's native context and returns its download GUID. |

## Tool behavior details

### Contexts and permissions

`browser.close_context` reports `closed: true` only after its windows have closed and Chromium has invalidated the context handles. If a window is still closing or refuses to close, the tool returns `closed: false` and `close_requested: true`; the context remains listed and usable until it closes or a 60-second close attempt expires. Use `browser.list_contexts` to check status and retry after resolving a beforeunload dialog. A discovered private context cannot be closed with this tool.

`context.permission` and `context.set_permission` use Chromium content settings. Common aliases include geolocation, notifications, camera, microphone, and popups; every registered setting name is accepted. `context.list_permissions` reports setting names and valid values. `page.permission_status` reports browser permission and combined OS/device status for geolocation, camera, microphone, and notifications on the current document. Normal Chromium and operating-system permission boundaries still apply.

### Element matching and native input

`page.find_elements` returns matches intersecting the viewport. Role and name use Blink's computed accessibility values in opted-in renderer processes. Native image roles are reported as the ARIA token `img`; a native `summary` inside `details` is reported as `button` when Blink omits its computed role. Elements hidden by an ancestor or fully clipped by overflow are excluded. `actionable` means the visible portion's center receives pointer input without another element covering it; Chromium hit testing accounts for inherited `pointer-events` and a child's explicit override. Coordinates use the `frame_viewport` space: pass the returned `frame_id` to native pointer tools for child-frame coordinates, or translate to main-frame coordinates.

`page.click` and `page.hover` use main-page viewport coordinates unless `frame_id` is provided. With a `frame_id`, the coordinates are relative to that live frame's viewport and input is sent to its native widget. If a child frame shares the main page's widget, frame-relative native input returns an error; translate those coordinates to the main-page viewport instead. `page.find_elements` name and text filters use case-sensitive substring matching.

`page.prepare_file_upload` supplies absolute local paths for the next file input chooser in the page's main frame. Follow it with a native `page.click` on the file input. Chromium's chooser completion grants the renderer access to the selected files, and the selection is consumed once.

### Accessibility and page storage

`page.accessibility_snapshot` includes native roles, names, descriptions, values, placeholders, hierarchy, expansion/required/busy/modal states, and available restriction, checked, heading, and set-position attributes.

`page.storage_state` reads local and session storage for the page frame's origin. `page.set_storage` changes string entries in either area through direct page evaluation. Both accept a `frame_id` from `page.frames`, so a client can inspect or update an open cross-origin frame under Chromium's same-origin rules. `context.local_storage_origins` and `context.clear_local_storage` operate on native storage keys, including keys without an open page and keys with partition identity.

### Downloads and dialogs

`page.download` starts a URL download associated with the page's main frame. Its optional `path` selects the target file. The returned native download GUID can be passed to `download.wait`, `download.cancel`, or matched in `context.downloads`.

`page.accept_dialog` and `page.dismiss_dialog` operate on the page's active JavaScript dialog through Chromium's dialog manager. The accept tool accepts an optional `prompt_text` override. `page.dialog_info` reads the active tab-modal alert, confirm, or prompt, and reports an active app-modal beforeunload dialog for the page.

### Network and console events

`page.console_messages` starts a native `WebContentsObserver` on first call and returns messages collected since the previous call, then clears that buffer. `page.network_events` uses the same observer to return completed resource loads since the previous call. Resource events include original and final URL, method, HTTP status, network error, MIME type, cache flag, body byte count, referrer, request ID, destination, total received bytes, and whether Chromium accessed the network.

Main-frame navigation lifecycle events include native request and response headers. A native URL loader throttle also records subresource request and response headers, including CORS-exempt request headers, and captures response bodies as Base64 bytes while teeing the original stream to the page only while the browser confirms a live observable page. Ineligible responses follow Chromium's original body path; request and response metadata may still be reported to the browser host and discarded there.

Each captured body is limited to 256 KiB. `captured_bytes`, `total_size`, `total_size_known`, and `truncated` describe the capture. At most 8 response bodies are captured at once across a renderer; additional bodies are forwarded without capture and report `capture_skipped`. Each tee reads through a 64 KiB pending buffer and a 32 KiB bounded forwarding pipe, so a slow page consumer eventually backpressures the network pipe. At most 256 KiB of forwarding-pipe capacity and 2 MiB of captured body strings are active per renderer. A body event reports `forwarded_bytes`, the bytes accepted by the page-facing pipe. Network event queues are limited to 16 MiB and 1024 events process-wide, with at most 16 queued events per page; older events are dropped when a limit is reached. Body events report `capture_error` and the low-level `capture_error_result` when a pipe operation fails. A generated request ID links request, response, and body events. Full-body retrieval is not provided; each body event contains only the finite captured prefix.

Redirected subresource bodies report `capture_skipped` with reason `redirected_response`; the redirect-following loader forwards them without installing the body tee. The resource-completion event separately reports final byte counts. Renderer interception requires both the `BrowserAgentService` feature and the `--enable-browser-agent-service` switch. It applies only to non-document requests from frame throttle providers with a local frame token; worker throttle providers do not enter this page-associated capture path.

### Browser event resource

`resources/list` exposes `chromium-mcp://browser/version`, `chromium-mcp://browser/contexts`, and the subscribable `chromium-mcp://browser/events` resource. `resources/read` returns JSON content with private, immediately stale cache hints. Unknown resource URIs return JSON-RPC `-32602`.

A `subscriptions/listen` POST opens an SSE response for `chromium-mcp://browser/events`. The server acknowledges the honored filter and sends `notifications/resources/updated` with the subscription ID when browser events change. Read the resource with `chromium-mcp://browser/events?after=<cursor>` to fetch event records. The journal retains the newest 256 records. Closing the SSE response stops its watch. Each listen response is capped at 256 KiB of notification bytes; at the cap, the server closes the response so the client can reopen it and read from its last cursor. A resource read reports `cursor_expired` and `oldest_cursor` if records before the oldest retained cursor were lost. GET is rejected with HTTP 405 for protocol revision `2026-07-28`. Malformed listen filters are rejected before a stream starts.

### Audit logs and tracing

With `--browser-agent-audit-log=<path>`, accepted MCP requests are appended as JSON Lines on a background task runner. Each entry records time, method, and tool name; request arguments and bearer tokens are not written.

With `--browser-agent-trace-dir=<directory>`, `browser.start_trace` starts Chromium tracing for the requested categories and `browser.stop_trace` writes the completed JSON trace to a uniquely named file in that directory.

Closing an in-flight HTTP request cancels its pending response. Native navigation and load waiters also detach immediately. Effects already handed to Chromium, such as a navigation or cookie write, cannot be rolled back.

## HTTP and JSON-RPC contract

For protocol revision `2026-07-28`, send one JSON-RPC request per POST with `Content-Type: application/json`, `Accept: application/json, text/event-stream`, `MCP-Protocol-Version`, and `Mcp-Method` headers. Every request includes `params._meta.io.modelcontextprotocol/protocolVersion` and `params._meta.io.modelcontextprotocol/clientCapabilities`. Calls to `tools/call` and `resources/read` also mirror the tool name or resource URI in `Mcp-Name`; unsafe values use the published Base64 sentinel encoding. GET returns HTTP 405.

Example tool call:

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

Example browser event subscription:

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

Use standard MCP JSON-RPC envelopes. The server returns standard tool results with structured content where appropriate. Malformed requests return standard `-32700`, `-32600`, `-32601`, or `-32602` errors. A tool that runs but cannot complete returns `isError: true` with `structuredContent.error` and a stable `error_code`: `invalid_argument`, `not_found`, `timeout`, or `browser_error`.

## Runtime and security

The loopback Streamable HTTP endpoint is owned by `BrowserAgentService`. It starts only when the `BrowserAgentService` feature and the explicit `--enable-browser-agent-service` switch are enabled, and a `--browser-agent-mcp-port=<port>` is supplied. Use port `0` to request an available loopback port. Chromium writes the selected endpoint and per-profile bearer token under `<profile>/BrowserAgentRuntime/`.

The AppImage stdio bridge reads the endpoint and token from that protected runtime directory and forwards standard MCP calls over loopback. Browser event streaming through `subscriptions/listen` is available to HTTP clients and is not currently forwarded by the stdio bridge.

Keep the endpoint on loopback and authenticate local clients so web pages cannot invoke it. An authenticated client can control the browser profile and use the full tool surface. Authentication identifies the client; it does not restrict tool capabilities. The browser process validates protocol syntax and tool arguments, but does not add agent-specific permission tiers or approval gates.

## Native control requirements

- Use browser-process APIs and existing Chromium interfaces directly.
- Evaluate JavaScript in the page main world through the browser-process `RenderFrameHost` API.
- Do not use CDP, remote debugging, DevTools protocol, Playwright, or a separate sidecar as the control path.
- Expose direct evaluation, page data, browser permissions, file selection, network, and storage capabilities to the authenticated local client.
- Let normal Chromium and operating-system permission behavior apply; do not add agent-specific policy or confirmation layers.
- Keep optional audit and trace output observational and non-blocking.

See `agent_protocol/` for schema tests and the official [MCP specification](https://modelcontextprotocol.io/specification/2026-07-28).
