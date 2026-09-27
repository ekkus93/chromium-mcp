# Chromium MCP Agent Browser Specification

Status: Draft v0.1  
Repository: `ekkus93/chromium-mcp`  
Target branch: `master`  
Primary goal: add a browser-native agent control plane to a Chromium fork and expose it through MCP using a Playwright-inspired tool surface.

## 1. Executive summary

This project turns a Chromium fork into an agent-controllable browser runtime. The browser itself remains the authority for tab state, frame state, input dispatch, permission decisions, sensitive-data redaction, download/upload mediation, and audit logging. MCP is the external protocol used by Claude Code or another agent host to request browser operations.

The design intentionally does not expose raw CDP, raw JavaScript execution, raw Mojo, or raw input primitives as the primary API. Instead, the MCP server exposes Playwright-inspired tools with structured arguments and structured responses. Tools operate on browser contexts, pages/tabs, locator specifications, element references, dialogs, downloads, uploads, network observations, traces, and audit logs.

The reason to fork Chromium is not merely to reimplement Selenium, CDP, or Playwright. The reason is to enforce policy inside the browser process, where the browser can see and control the real security-relevant state: current origin, focused frame, active tab mode, permission state, password/autofill classification, file chooser state, downloads, browser dialogs, and user interaction.

## 2. Goals

1. Provide a Chromium-native agent control plane that can be exposed through MCP.
2. Model the common Playwright feature set: browser contexts, pages, locators, actions, waiting, screenshots, dialogs, downloads/uploads, network observation, console messages, emulation, tracing, and storage state.
3. Keep the Chromium patch surface small and isolated so the fork can be rebased.
4. Prefer semantic locators and accessibility-driven observation over raw CSS/XPath or raw DOM dumps.
5. Enforce tab modes and permission policy in the browser process before any action is dispatched.
6. Protect sensitive fields, sensitive documents, user keystrokes, user clipboard data, downloads, uploads, and high-impact form submissions.
7. Produce mandatory browser-native audit logs for all agent actions and policy decisions.
8. Support local-first operation through a sidecar MCP server that speaks to Chromium over a narrow local IPC interface.
9. Keep the MCP tool contract stable enough that agents can use it without depending on Chromium internals.
10. Provide enough structure that future local model agents, Claude Code, desktop assistants, or testing agents can share the same browser-control substrate.

## 3. Non-goals

1. Do not expose full CDP as MCP.
2. Do not expose arbitrary browser-process Mojo messages as MCP.
3. Do not expose arbitrary JavaScript evaluation as a default capability.
4. Do not allow the agent to bypass browser permission prompts, origin policy, sandboxing, or Safe Browsing decisions.
5. Do not allow the agent to silently read password fields, hidden credentials, authentication codes, cookies, authorization headers, local storage secrets, or browser profile secrets.
6. Do not allow arbitrary filesystem upload/download paths controlled by the agent.
7. Do not design this as a pure test automation system. Playwright is the API inspiration, but this project is an agentic co-browsing runtime.
8. Do not require a cloud service for core local control.
9. Do not make the renderer process authoritative for policy decisions.
10. Do not optimize for maximum possible browser control at the expense of user agency and auditability.

## 4. High-level architecture

```text
Agent host / Claude Code / local model
        |
        | MCP
        v
chromium-mcp sidecar server
        |
        | local authenticated IPC
        v
Chromium browser process
  BrowserAgentService
        |
        +-- ContextRegistry
        +-- PageRegistry
        +-- LocatorResolver
        +-- ActionDispatcher
        +-- PolicyEngine
        +-- SensitiveDataClassifier
        +-- PermissionBroker
        +-- DownloadUploadBroker
        +-- NetworkObserver
        +-- DialogBroker
        +-- TraceRecorder
        +-- AuditLog
        |
        +-- WebContents / RenderFrameHost / Accessibility / Input / Downloads
```

The sidecar process owns MCP protocol concerns. Chromium owns browser semantics and enforcement. This separation keeps the Chromium fork smaller and allows the MCP server to be written in TypeScript, Rust, Go, or Python without embedding a full MCP stack inside Chromium C++.

## 5. Process placement

### 5.1 Browser process

The browser process contains the authoritative `BrowserAgentService`. It must be implemented in browser-side Chromium code, not renderer-side code. It owns:

- Page and context handles.
- Tab mode state.
- Action admission decisions.
- Action dispatch to browser input APIs.
- Accessibility snapshots.
- Redacted DOM summaries.
- Frame and origin metadata.
- Download and upload policy mediation.
- Dialog handling.
- Network observation and redaction.
- Trace and audit artifacts.

### 5.2 Renderer process

Renderer code may provide observation helpers, but it must not become the policy authority. Renderer-provided data is untrusted. The renderer can be compromised by web content, so any renderer-originated claim about origin, sensitivity, file paths, policy, or user authorization must be validated in the browser process.

### 5.3 Sidecar MCP server

The sidecar MCP server exposes MCP tools and maps them to a narrow browser-agent IPC protocol. The sidecar should be restartable without killing the browser. It should not contain policy that the browser must trust. It may perform schema validation, request shaping, and user-facing error formatting, but Chromium must still revalidate every action.

### 5.4 Local IPC

Preferred local transports:

1. Unix domain socket on Linux/macOS.
2. Named pipe on Windows.
3. Loopback HTTP only if it is bound to localhost, requires a session capability, and rejects cross-origin browser requests.
4. Mojo only if integrating the sidecar more deeply into Chromium is acceptable.

The IPC layer must authenticate the sidecar. At minimum use an unguessable per-profile session capability created by Chromium and passed to the sidecar at launch. Do not rely solely on localhost as an authorization boundary.

## 6. Security model

### 6.1 Trust boundaries

- User: ultimate authority.
- Browser process: trusted computing base for policy enforcement.
- Sidecar MCP server: trusted only as a local adapter, not as the final policy authority.
- Agent host: untrusted caller with limited capabilities.
- Renderer process: untrusted page execution environment.
- Web page: adversarial input.
- Extensions: separate risk surface; initially unsupported or heavily restricted.

### 6.2 Core policy principles

1. Every mutating action must pass through `PolicyEngine` before dispatch.
2. Every action must be associated with a page, context, origin, actor, tool name, reason, and action ID.
3. Page content must be assumed prompt-injection-capable.
4. Browser state exported to the agent must be redacted by default.
5. Agent actions must be visible and auditable.
6. Sensitive operations must require explicit user approval unless a narrower allow policy exists.
7. Policy denial must be fail-closed.
8. Ambiguous sensitivity must default to protected.

### 6.3 Protected classes of data

The agent must not receive or manipulate these by default:

- Password field values.
- One-time passcodes.
- Authentication secrets.
- Cookies and session identifiers.
- Authorization and proxy authorization headers.
- Raw local storage/session storage on sensitive origins.
- Browser profile secret stores.
- Payment card numbers and CVV fields.
- Government identifiers and tax identifiers when recognized.
- Hidden form fields on sensitive flows.
- User keystrokes entered while the tab is in manual or sensitive mode.
- Clipboard contents while a sensitive field or sensitive origin is active.
- Arbitrary files from the user filesystem.

### 6.4 User approval classes

Require explicit user approval for:

- File upload.
- File download save location.
- Permission grants for camera, microphone, location, notifications, clipboard, MIDI, USB, serial, HID, Bluetooth, and local network access.
- Login form submission.
- Payment form submission.
- Account creation.
- Account deletion or destructive account changes.
- Password changes.
- OAuth grants and consent screens.
- Data export/import operations.
- Network interception/modification if enabled.
- Arbitrary JavaScript evaluation if enabled.

## 7. Page modes

Each page/tab has an agent-control mode. PolicyEngine must consult this mode before observation or action.

| Mode | Observation | Action | Intended use |
|---|---|---|---|
| `MANUAL` | URL/title/basic metadata only | Blocked | User-controlled tab. |
| `ASSISTED` | Redacted semantic snapshots | Low-risk actions allowed with audit | Human-in-the-loop browsing. |
| `AGENT` | Redacted semantic snapshots | Allowed within policy | Autonomous bounded browsing. |
| `SENSITIVE` | Minimal metadata only | Blocked except explicit user-approved safe operations | Passwords, banking, health, identity, private docs. |

Mode transitions must be logged. The browser UI should show the current mode clearly on each controlled tab.

## 8. Handle model

MCP tools should use opaque handles:

- `context_id`: browser context/profile/session handle.
- `page_id`: page/tab handle.
- `frame_id`: frame handle.
- `element_id`: short-lived resolved element handle.
- `dialog_id`: pending dialog handle.
- `download_id`: download handle.
- `upload_file_id`: user-approved file handle.
- `trace_id`: trace session/artifact handle.
- `action_id`: audit/action handle.

Handles must be scoped. An `element_id` must be scoped to page, frame, origin, document version, and generation. It must expire on navigation, significant DOM replacement, or timeout.

## 9. Locator model

Use Playwright as the locator inspiration, but serialize locator specifications for MCP.

```json
{
  "kind": "role",
  "role": "button",
  "name": "Continue",
  "exact": false
}
```

Supported locator kinds:

1. `role`
2. `label`
3. `text`
4. `placeholder`
5. `alt_text`
6. `title`
7. `test_id`
8. `css`
9. `xpath`
10. `nth`
11. `chain`
12. `near_text`
13. `semantic`

Preferred order for agent-generated locators:

1. Accessibility role and accessible name.
2. Label text.
3. Visible text.
4. Placeholder text.
5. Alt/title/test ID.
6. CSS.
7. XPath.

CSS and XPath are supported for parity, but should be treated as less stable and less user-semantic.

## 10. Actionability model

Before actions such as click, fill, type, check, select, or drag/drop, the browser must verify that the target is actionable.

Actionability checks:

- Element resolves uniquely when `strict=true`.
- Element is attached.
- Element is visible unless action explicitly allows hidden targets.
- Element is stable.
- Element is enabled.
- Element is not obscured by an unrelated overlay for pointer actions.
- Element is in the active page/frame.
- Element is within allowed tab mode.
- Element is not sensitive unless the operation is approved.
- Any resulting navigation/submission/download/upload is policy-admitted.

Trial mode must be supported for actions. `trial=true` performs resolution and policy checks without dispatching input.

## 11. MCP tool naming

Use dotted names grouped by domain:

- `browser.*`
- `context.*`
- `page.*`
- `locator.*`
- `dialog.*`
- `download.*`
- `upload.*`
- `network.*`
- `console.*`
- `trace.*`
- `audit.*`
- `policy.*`

Tool names should be stable and should not expose Chromium internal class names.

## 12. Standard request fields

Most action tools should accept:

```json
{
  "context_id": "ctx_...",
  "page_id": "page_...",
  "timeout_ms": 5000,
  "strict": true,
  "trial": false,
  "reason": "Agent-readable reason for audit log",
  "user_visible": true
}
```

`reason` is mandatory for mutating actions. It does not authorize the action; it improves auditability.

## 13. Standard response envelope

All tools should return structured responses:

```json
{
  "ok": true,
  "action_id": "act_...",
  "policy": {
    "decision": "allowed",
    "rules_applied": ["tab_mode_agent", "target_non_sensitive"]
  },
  "result": {},
  "page_state": {
    "url": "https://example.com/",
    "title": "Example",
    "load_state": "idle"
  }
}
```

Failure format:

```json
{
  "ok": false,
  "action_id": "act_...",
  "error": {
    "code": "locator_not_found",
    "message": "No visible button named Continue found before timeout.",
    "retryable": true
  },
  "debug": {
    "similar_elements": []
  }
}
```

## 14. Tool surface: P0

P0 is the minimum useful agent-control surface.

### 14.1 Browser/context

- `browser.version`
- `browser.capabilities`
- `browser.list_contexts`
- `context.new`
- `context.close`
- `context.get_storage_state`
- `context.set_default_timeout`

### 14.2 Page/tab

- `page.list`
- `page.new`
- `page.close`
- `page.bring_to_front`
- `page.goto`
- `page.reload`
- `page.go_back`
- `page.go_forward`
- `page.get_info`
- `page.set_mode`
- `page.wait_for_load_state`
- `page.wait_for_url`

### 14.3 Locator and observation

- `locator.query`
- `locator.describe`
- `locator.highlight`
- `page.visible_elements`
- `page.snapshot_accessibility`
- `page.screenshot`
- `page.text_content`

### 14.4 Actions

- `page.click`
- `page.double_click`
- `page.hover`
- `page.fill`
- `page.type_text`
- `page.press`
- `page.check`
- `page.uncheck`
- `page.select_option`
- `page.focus`
- `page.clear`
- `page.wait_for_selector`

### 14.5 Dialogs, tracing, audit

- `dialog.list`
- `dialog.accept`
- `dialog.dismiss`
- `trace.start`
- `trace.stop`
- `trace.export`
- `audit.list_actions`
- `audit.get_action`

## 15. Tool surface: P1

P1 completes the common Playwright parity layer.

- `page.drag_and_drop`
- `page.frame_tree`
- `page.content` with elevated privilege and redaction policy.
- `page.wait_for_event`
- `download.list`
- `download.cancel`
- `download.save_as`
- `upload.set_files`
- `console.messages`
- `network.list_requests`
- `network.wait_for_request`
- `network.wait_for_response`
- `context.set_viewport`
- `context.set_locale`
- `context.set_timezone`
- `context.set_offline`
- `context.get_permissions`
- `context.request_permission_grant`

## 16. Tool surface: P2 advanced/debug

P2 is powerful and must be gated by policy/profile.

- `network.get_response_body`
- `network.route`
- `network.unroute`
- `network.fulfill`
- `page.evaluate_safe`
- `page.evaluate` disabled by default.
- `context.set_extra_http_headers`
- `context.set_geolocation`
- `context.import_storage_state`
- `har.start`
- `har.stop`
- `video.start`
- `video.stop`

## 17. Selected tool schemas

### 17.1 `page.goto`

```json
{
  "page_id": "page_123",
  "url": "https://example.com",
  "wait_until": "domcontentloaded",
  "timeout_ms": 30000,
  "reason": "Open the target page for the requested task."
}
```

Allowed `wait_until` values:

- `commit`
- `domcontentloaded`
- `load`
- `networkidle`

### 17.2 `locator.query`

```json
{
  "page_id": "page_123",
  "locator": {
    "kind": "role",
    "role": "button",
    "name": "Continue",
    "exact": false
  },
  "max_results": 10,
  "timeout_ms": 5000
}
```

Return element summaries including role, accessible name, bounds, enabled state, visible state, sensitivity classification, frame ID, and short-lived `element_id`.

### 17.3 `page.click`

```json
{
  "page_id": "page_123",
  "locator": {
    "kind": "role",
    "role": "button",
    "name": "Continue"
  },
  "button": "left",
  "click_count": 1,
  "modifiers": [],
  "strict": true,
  "trial": false,
  "timeout_ms": 5000,
  "reason": "Continue to the next step after reviewing the page."
}
```

### 17.4 `page.fill`

```json
{
  "page_id": "page_123",
  "locator": {
    "kind": "label",
    "text": "Search"
  },
  "text": "chromium mcp browser",
  "strict": true,
  "timeout_ms": 5000,
  "reason": "Fill the search box with the requested query."
}
```

`page.fill` must refuse sensitive fields unless approved. `page.type_text` should follow the same policy but dispatch keystroke-like input instead of replacing contents.

### 17.5 `page.snapshot_accessibility`

```json
{
  "page_id": "page_123",
  "interesting_only": true,
  "max_nodes": 500,
  "redact_sensitive": true
}
```

### 17.6 `download.save_as`

```json
{
  "download_id": "download_123",
  "destination_id": "approved_destination_456",
  "reason": "Save the downloaded artifact requested by the user."
}
```

The agent never provides arbitrary local paths. It uses browser/user-approved destination handles.

### 17.7 `upload.set_files`

```json
{
  "page_id": "page_123",
  "locator": {
    "kind": "label",
    "text": "Upload file"
  },
  "files": [
    { "upload_file_id": "file_approved_123" }
  ],
  "reason": "Attach the user-approved file."
}
```

## 18. Browser UI requirements

The Chromium fork needs visible UI for agent control. At minimum:

1. Per-tab mode indicator.
2. Agent active indicator.
3. Last action summary.
4. Permission prompt for high-risk actions.
5. Audit log viewer.
6. Trace export control.
7. Emergency stop button that cancels pending and future agent actions.
8. Sidecar connection status.
9. Highlight overlay for locator/action targets.
10. Clear indication when page content is being observed by an agent.

## 19. Audit log

Every tool call must create an audit entry, including denied calls. Required fields:

- `action_id`
- `timestamp`
- `actor`
- `tool_name`
- `context_id`
- `page_id`
- `frame_id` when applicable
- `origin`
- `url`
- `page_mode`
- redacted arguments
- locator spec when applicable
- resolved target summary when applicable
- policy decision
- rules applied
- user approval ID when applicable
- dispatch result
- error code when applicable
- resulting navigation/download/dialog/form-submission event when applicable

Audit records should be append-only for a session. Export format should be JSON Lines plus optional trace artifacts.

## 20. Trace model

Tracing should be mandatory for agent sessions, even if screenshots are configurable. Trace events should include:

- Tool calls.
- Policy decisions.
- Actionability checks.
- Locator resolution attempts.
- Screenshots before/after mutating actions when enabled.
- Redacted accessibility snapshots.
- Navigation events.
- Dialog events.
- Download/upload events.
- Network request metadata when enabled.
- Console messages when enabled.

Trace artifacts must avoid storing secrets by default. Sensitive text should be redacted before persistence.

## 21. Network model

P0/P1 network functionality should be observation-first:

- recent requests
- recent responses
- wait for request
- wait for response
- status/method/URL/resource type/timing

Headers and bodies are sensitive. By default:

- Redact cookies.
- Redact auth headers.
- Redact tokens.
- Redact request bodies for login/payment/account forms.
- Do not return cross-origin response bodies unless policy allows.

Network interception belongs in P2/debug mode only.

## 22. JavaScript evaluation model

`page.evaluate` is not a default capability. Provide `page.evaluate_safe` first, where `script_id` references a browser-shipped allowlisted script such as:

- `document_metadata`
- `selection_text`
- `element_computed_label_debug`
- `form_summary_redacted`
- `performance_summary`

Raw JavaScript evaluation, if implemented, must require an explicit developer/debug capability profile and must be fully audited.

## 23. Storage-state model

Implement Playwright-like storage-state workflows, but gate them carefully.

- Export storage only from approved contexts.
- Redact or omit sensitive origins by policy.
- Import storage only with user/developer approval.
- Audit all storage import/export.
- Never leak browser profile secrets outside the approved artifact.

## 24. Download/upload model

Downloads and uploads are high-risk because they cross the browser/filesystem boundary.

Downloads:

- Browser detects download creation.
- Agent may list download metadata.
- Agent may request save/cancel.
- User approves destination or policy provides a sandboxed destination.
- Agent never writes arbitrary filesystem paths.

Uploads:

- User or policy pre-approves file handles.
- Agent may attach only approved file handles.
- Browser displays selected filenames before submission when appropriate.
- Submission after upload may require separate approval.

## 25. Error taxonomy

Initial error codes:

- `invalid_request`
- `unknown_context`
- `unknown_page`
- `unknown_frame`
- `unknown_element`
- `stale_element`
- `locator_not_found`
- `locator_ambiguous`
- `not_visible`
- `not_enabled`
- `not_editable`
- `not_stable`
- `action_blocked_by_policy`
- `requires_user_approval`
- `approval_denied`
- `timeout`
- `navigation_failed`
- `download_blocked`
- `upload_blocked`
- `dialog_required`
- `sensitive_data_redacted`
- `sidecar_not_authorized`
- `browser_disconnected`
- `internal_error`

Errors should identify whether retrying is useful and should include safe debugging hints such as similar visible elements.

## 26. Implementation layout recommendation

Chromium-side directories:

```text
chrome/browser/agent/
  browser_agent_service.{h,cc}
  browser_agent_context_registry.{h,cc}
  browser_agent_page_registry.{h,cc}
  browser_agent_policy_engine.{h,cc}
  browser_agent_locator_resolver.{h,cc}
  browser_agent_action_dispatcher.{h,cc}
  browser_agent_audit_log.{h,cc}
  browser_agent_trace_recorder.{h,cc}
  browser_agent_sensitive_data_classifier.{h,cc}
  browser_agent_download_upload_broker.{h,cc}
  browser_agent_dialog_broker.{h,cc}
  browser_agent_network_observer.{h,cc}
  browser_agent.mojom
```

Sidecar directories:

```text
mcp-sidecar/
  src/server.ts
  src/browser_client.ts
  src/tools/
  src/schemas/
  src/policy_types.ts
  test/
```

Shared protocol/schema:

```text
agent_protocol/
  browser_agent_protocol.schema.json
  tool_schemas/
  generated/
```

Docs/tests:

```text
docs/
  CHROMIUM_MCP_AGENT_BROWSER_SPEC.md
  CHROMIUM_MCP_AGENT_BROWSER_TODO.md
  SECURITY_MODEL.md
  TOOL_CONTRACT.md
  TEST_PLAN.md
```

## 27. Build and runtime flags

Initial flags:

- `--enable-browser-agent-service`
- `--browser-agent-sidecar=/path/to/sidecar`
- `--browser-agent-profile=dev|local|test|restricted`
- `--browser-agent-socket=/path/to/socket`
- `--browser-agent-audit-log=/path/to/log.jsonl`
- `--browser-agent-trace-dir=/path/to/traces`
- `--browser-agent-disable-raw-evaluate`

The service must be disabled by default until the feature is mature.

## 28. Testing strategy

### 28.1 Unit tests

- Locator parsing and validation.
- Locator resolution against synthetic accessibility trees.
- Policy decisions for every tab mode.
- Sensitive data classification.
- Audit log redaction.
- Error mapping.
- Tool schema validation.

### 28.2 Browser tests

- Create context/page.
- Navigate and wait for lifecycle states.
- Resolve role/text/label locators.
- Click/fill/type/select/check/uncheck.
- Block actions in manual/sensitive modes.
- Redact password field values.
- Require approval for downloads/uploads.
- Handle dialogs.
- Capture screenshot and accessibility snapshot.
- Produce audit entries for success and denial.

### 28.3 Sidecar integration tests

- MCP tool list.
- JSON schema validation.
- Successful browser connection.
- Reconnection after sidecar restart.
- Tool call to Chromium IPC mapping.
- Structured error propagation.

### 28.4 End-to-end tests

Use local test pages served from a deterministic fixture server. Cover:

- Simple navigation and search.
- Login form redaction and blocked submission.
- File upload requiring approval.
- Download requiring approved destination.
- Dialog accept/dismiss.
- Multi-frame page.
- Popup/new tab.
- Network request observation.
- Trace export.

## 29. Minimum acceptance criteria for MVP

MVP is complete when:

1. Chromium builds with the agent service behind a flag.
2. Sidecar starts and exposes MCP tools.
3. The sidecar can connect to the browser over authenticated local IPC.
4. An agent can create/list pages, navigate, observe visible elements, click, fill, type, press, and wait.
5. Locators support role, text, label, placeholder, CSS, and XPath.
6. Accessibility snapshots are redacted.
7. Password fields are never returned to the agent.
8. Manual and sensitive page modes block actions.
9. Downloads/uploads require approval or sandbox policy.
10. Dialogs can be listed and accepted/dismissed under policy.
11. Every tool call produces an audit entry.
12. Trace start/stop/export works for an agent session.
13. Browser tests cover allowed and denied actions.
14. Documentation describes the tool contract and policy model.

## 30. Open design questions

1. Which sidecar implementation language should be canonical first: TypeScript, Rust, or Python?
2. Should the browser launch the sidecar, or should the sidecar attach to an already-running browser?
3. Should the first IPC transport be Unix socket/named pipe or Mojo?
4. How much of the accessibility tree should be exposed by default?
5. What is the first user approval UX: modal, infobar, permission chip, or side panel?
6. Should extension support be completely disabled in controlled contexts for MVP?
7. Should contexts map to Chromium profiles, off-the-record profiles, or a lighter logical session layer?
8. How should long-running waits and cancellation be represented in MCP?
9. Should P0 include screenshots as binary MCP resources or file artifacts referenced by handle?
10. How should this fork stay rebased against upstream Chromium with minimal conflicts?

## 31. Recommended first implementation slice

Start with the smallest useful path:

1. Add `BrowserAgentService` behind a feature flag.
2. Add page registry and basic tab listing.
3. Add local IPC handshake and `browser.version`.
4. Add `page.list`, `page.get_info`, `page.goto`, and `page.screenshot`.
5. Add accessibility snapshot and visible element list.
6. Add role/text locator resolution.
7. Add `page.click` and `page.fill` with policy checks.
8. Add tab mode enforcement.
9. Add audit log.
10. Add sidecar MCP wrapper around those tools.

That slice proves the entire architecture without committing to advanced network interception, raw evaluation, or deep renderer changes.

## 32. Summary

The project should implement a browser-native, policy-enforced, Playwright-inspired MCP control plane for Chromium. The core value is not raw automation. The core value is safe, inspectable, native co-browsing: semantic observation, controlled actions, visible tab modes, sensitive-data redaction, permissioned filesystem crossings, and mandatory auditability.
