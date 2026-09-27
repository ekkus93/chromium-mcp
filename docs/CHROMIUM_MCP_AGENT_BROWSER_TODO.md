# Chromium MCP Agent Browser TODO

Status: Draft v0.1  
Repository: `ekkus93/chromium-mcp`  
Canonical companion spec: `docs/CHROMIUM_MCP_AGENT_BROWSER_SPEC.md`  
Goal: implement a Chromium-native, Playwright-inspired MCP browser-control plane with browser-process policy enforcement.

## Completion policy

This TODO is complete only when the implementation, tests, documentation, and qualification evidence satisfy the acceptance criteria in the companion spec. Checkboxes represent implementation truth, not intent. Do not mark a task complete until the code is merged, tests pass, and any relevant docs are updated.

## Working rules

- Keep Chromium fork changes isolated and rebaseable.
- Prefer one coherent vertical slice over many tiny unrelated changes.
- Do not expose raw CDP, raw Mojo, or raw JavaScript evaluation as the default API.
- Browser process policy is authoritative.
- Renderer-provided data is untrusted.
- Every action is audited, including denied actions.
- Sensitive data redaction is mandatory before agent observation.
- User approval is required for high-risk operations.
- MCP sidecar adapts protocol; Chromium enforces browser policy.

---

## Phase 0 — Repository orientation and project scaffolding

- [ ] Inspect current repository layout and identify the Chromium checkout/fork structure.
- [ ] Document the expected developer workflow for syncing/rebasing upstream Chromium.
- [ ] Document supported host platforms for initial development.
- [ ] Add or confirm top-level project docs directory.
- [ ] Add project README section that links to this TODO and the spec.
- [ ] Add a short architecture overview to README.
- [ ] Identify the Chromium build args used by this repo.
- [ ] Identify the primary GN/Ninja targets needed for desktop Chromium build and tests.
- [ ] Create a development branch policy for large Chromium changes if the repo requires it.
- [ ] Confirm CI capacity for documentation-only commits, sidecar tests, and Chromium tests.
- [ ] Add a `docs/SECURITY_MODEL.md` stub if not already present.
- [ ] Add a `docs/TOOL_CONTRACT.md` stub if not already present.
- [ ] Add a `docs/TEST_PLAN.md` stub if not already present.

### Phase 0 acceptance

- [ ] Repository docs clearly explain the Chromium MCP Agent Browser project.
- [ ] Build/test entry points are documented.
- [ ] Spec and TODO are discoverable from README or equivalent project index.

---

## Phase 1 — Feature flag and browser-process service skeleton

- [ ] Add a Chromium feature flag for the browser agent service.
- [ ] Add runtime switch `--enable-browser-agent-service`.
- [ ] Add runtime switch `--browser-agent-profile=dev|local|test|restricted`.
- [ ] Add runtime switch `--browser-agent-audit-log=<path>`.
- [ ] Add runtime switch `--browser-agent-trace-dir=<path>`.
- [ ] Add runtime switch `--browser-agent-disable-raw-evaluate`.
- [ ] Add `chrome/browser/agent/` directory.
- [ ] Add `BrowserAgentService` class.
- [ ] Wire `BrowserAgentService` lifetime to browser/profile lifetime.
- [ ] Ensure service is disabled by default.
- [ ] Ensure service cannot run unless explicitly enabled.
- [ ] Add skeleton unit test target for `chrome/browser/agent`.
- [ ] Add basic service startup/shutdown tests.
- [ ] Add structured logging for service start/stop.
- [ ] Add developer documentation for enabling the service locally.

### Phase 1 acceptance

- [ ] Chromium builds with the service skeleton behind a flag.
- [ ] Service startup and shutdown are covered by tests.
- [ ] With the flag disabled, browser behavior is unchanged.

---

## Phase 2 — Browser-agent protocol and IPC foundation

- [ ] Choose initial IPC transport: Unix domain socket/named pipe, loopback HTTP, or Mojo.
- [ ] Document the transport decision in `docs/TOOL_CONTRACT.md`.
- [ ] Define browser-agent protocol version string.
- [ ] Define common request envelope.
- [ ] Define common response envelope.
- [ ] Define structured error envelope.
- [ ] Define `context_id`, `page_id`, `frame_id`, `element_id`, `action_id`, `dialog_id`, `download_id`, `trace_id` handle formats.
- [ ] Implement sidecar authentication handshake.
- [ ] Generate or load a per-profile session capability.
- [ ] Ensure session capability is not exposed to web pages.
- [ ] Reject unauthenticated sidecar connections.
- [ ] Add connection lifecycle state to the browser agent service.
- [ ] Add disconnect handling.
- [ ] Add cancellation primitive for pending actions.
- [ ] Add protocol schema files under `agent_protocol/`.
- [ ] Add protocol schema validation tests.

### Phase 2 acceptance

- [ ] Sidecar can connect to browser service locally.
- [ ] Unauthorized connections are rejected.
- [ ] Protocol version and capability negotiation work.
- [ ] Errors are structured and stable.

---

## Phase 3 — Sidecar MCP server skeleton

- [ ] Choose sidecar implementation language.
- [ ] Create `mcp-sidecar/` package.
- [ ] Add MCP server bootstrap.
- [ ] Add browser IPC client wrapper.
- [ ] Add generated or handwritten JSON schemas for all P0 tools.
- [ ] Implement `browser.version` tool.
- [ ] Implement `browser.capabilities` tool.
- [ ] Implement tool registry.
- [ ] Implement standard argument validation.
- [ ] Implement standard response mapping.
- [ ] Implement sidecar reconnect behavior.
- [ ] Implement sidecar shutdown handling.
- [ ] Add sidecar unit tests.
- [ ] Add sidecar integration test with a fake browser-agent service.
- [ ] Add package scripts for lint/test/build.
- [ ] Document sidecar startup modes.

### Phase 3 acceptance

- [ ] MCP server starts locally.
- [ ] MCP tool list includes P0 stubs.
- [ ] `browser.version` returns Chromium/agent protocol metadata.
- [ ] Sidecar tests pass without requiring a full Chromium build.

---

## Phase 4 — Context and page registries

- [ ] Implement `ContextRegistry`.
- [ ] Define context lifecycle states.
- [ ] Map browser profile/off-the-record profile/session concept to `context_id`.
- [ ] Implement `browser.list_contexts`.
- [ ] Implement `context.new`.
- [ ] Implement `context.close`.
- [ ] Implement `context.set_default_timeout`.
- [ ] Implement `context.get_storage_state` stub with policy denial until storage model is complete.
- [ ] Implement `PageRegistry`.
- [ ] Map Chromium `WebContents` to `page_id`.
- [ ] Detect tab creation and closure.
- [ ] Detect popup/new-window creation.
- [ ] Implement `page.list`.
- [ ] Implement `page.new`.
- [ ] Implement `page.close`.
- [ ] Implement `page.bring_to_front`.
- [ ] Implement `page.get_info` with URL/title/origin/load state/page mode/focused-frame metadata.
- [ ] Add tests for page registry lifecycle.
- [ ] Add tests for stale page handles.

### Phase 4 acceptance

- [ ] Sidecar can list contexts and pages.
- [ ] New tabs can be created and closed.
- [ ] Page handles become invalid after closure.
- [ ] Page metadata is accurate and redacted where required.

---

## Phase 5 — Page modes and policy engine foundation

- [ ] Add page mode enum: `MANUAL`, `ASSISTED`, `AGENT`, `SENSITIVE`.
- [ ] Implement per-page mode storage.
- [ ] Add `page.set_mode`.
- [ ] Add browser UI indicator for current page mode.
- [ ] Add mode transition audit events.
- [ ] Implement `PolicyEngine` class.
- [ ] Add policy decision type: `allowed`, `blocked`, `requires_user_approval`.
- [ ] Add policy rule identifiers.
- [ ] Add policy check for observation tools.
- [ ] Add policy check for mutating action tools.
- [ ] Add policy check for filesystem-crossing tools.
- [ ] Add policy check for permission-grant tools.
- [ ] Add default policy matrix for all page modes.
- [ ] Add emergency-stop state.
- [ ] Ensure emergency stop cancels pending actions.
- [ ] Add tests for each page mode.
- [ ] Add tests for emergency stop.
- [ ] Add docs for page modes and policy matrix.

### Phase 5 acceptance

- [ ] `MANUAL` mode blocks mutating actions.
- [ ] `SENSITIVE` mode blocks observation beyond minimal metadata.
- [ ] `ASSISTED` and `AGENT` modes permit low-risk actions subject to policy.
- [ ] All policy decisions are test-covered.

---

## Phase 6 — Audit log

- [ ] Implement `AuditLog` component.
- [ ] Define audit entry schema.
- [ ] Log every tool call.
- [ ] Log denied calls.
- [ ] Log policy decision and rules applied.
- [ ] Log redacted arguments.
- [ ] Log page/context/frame/origin metadata.
- [ ] Log resolved locator target summaries.
- [ ] Log action result or error code.
- [ ] Log navigation/download/dialog side effects.
- [ ] Implement JSON Lines export.
- [ ] Implement in-memory bounded query API.
- [ ] Implement `audit.list_actions`.
- [ ] Implement `audit.get_action`.
- [ ] Redact sensitive fields in audit output.
- [ ] Add unit tests for audit redaction.
- [ ] Add browser tests proving allowed and denied actions are logged.

### Phase 6 acceptance

- [ ] Every MCP tool call produces an audit entry.
- [ ] Redaction is applied before persistence and export.
- [ ] Audit queries return structured action records.

---

## Phase 7 — Navigation and lifecycle tools

- [ ] Implement `page.goto`.
- [ ] Implement `page.reload`.
- [ ] Implement `page.go_back`.
- [ ] Implement `page.go_forward`.
- [ ] Implement lifecycle state tracking: `commit`, `domcontentloaded`, `load`, `networkidle`.
- [ ] Implement `page.wait_for_load_state`.
- [ ] Implement `page.wait_for_url`.
- [ ] Add timeout handling.
- [ ] Add cancellation handling.
- [ ] Ensure navigation failures map to structured errors.
- [ ] Audit all navigation attempts.
- [ ] Add browser tests for successful navigation.
- [ ] Add browser tests for navigation timeout.
- [ ] Add browser tests for blocked navigation in `MANUAL`/`SENSITIVE` modes.

### Phase 7 acceptance

- [ ] Agent can navigate a page and wait for lifecycle state.
- [ ] Navigation obeys page mode policy.
- [ ] Timeouts and failures are structured and audited.

---

## Phase 8 — Accessibility snapshots and visible element observation

- [ ] Implement redacted accessibility snapshot extraction.
- [ ] Implement `page.snapshot_accessibility`.
- [ ] Add `interesting_only` support.
- [ ] Add `max_nodes` support.
- [ ] Add sensitivity annotations to nodes.
- [ ] Implement `page.visible_elements`.
- [ ] Include role/name/bounds/enabled/visible/stable/editable/sensitive metadata.
- [ ] Implement `page.text_content` returning visible redacted text.
- [ ] Implement `page.screenshot`.
- [ ] Return screenshots as MCP resources or file handles.
- [ ] Ensure screenshot behavior is policy-gated for sensitive pages.
- [ ] Add redaction tests for password fields.
- [ ] Add tests for hidden elements.
- [ ] Add tests for iframe content observation.
- [ ] Add tests for `SENSITIVE` mode minimal observation.

### Phase 8 acceptance

- [ ] Agent can observe semantic page state.
- [ ] Sensitive values are redacted.
- [ ] Screenshots and snapshots obey page mode policy.

---

## Phase 9 — Sensitive data classifier

- [ ] Implement `SensitiveDataClassifier` component.
- [ ] Classify input type password as sensitive.
- [ ] Classify OTP/auth-code fields as sensitive where detectable.
- [ ] Classify payment fields as sensitive where detectable.
- [ ] Classify account-change forms as sensitive where detectable.
- [ ] Classify login forms as sensitive for submit decisions.
- [ ] Classify OAuth/consent flows as approval-required.
- [ ] Integrate autofill field classification where available.
- [ ] Integrate accessibility role/name heuristics.
- [ ] Integrate origin/URL heuristics for high-risk flows.
- [ ] Add ambiguous classification fallback to protected.
- [ ] Add classifier debug output for tests only.
- [ ] Add unit tests for common sensitive field patterns.
- [ ] Add browser tests for password/payment/login/account forms.

### Phase 9 acceptance

- [ ] Password values are never exposed.
- [ ] Sensitive fields block fill/type unless approved by policy.
- [ ] Login/payment/account submissions require approval.
- [ ] Ambiguous sensitive forms fail closed.

---

## Phase 10 — Locator resolver

- [ ] Implement `LocatorSpec` parser and validator.
- [ ] Implement `role` locator.
- [ ] Implement `label` locator.
- [ ] Implement `text` locator.
- [ ] Implement `placeholder` locator.
- [ ] Implement `alt_text` locator.
- [ ] Implement `title` locator.
- [ ] Implement `test_id` locator.
- [ ] Implement `css` locator.
- [ ] Implement `xpath` locator.
- [ ] Implement `nth` locator.
- [ ] Implement `chain` locator.
- [ ] Implement locator strictness rules.
- [ ] Implement locator timeout/auto-wait.
- [ ] Implement short-lived `element_id` handles.
- [ ] Expire element handles on navigation.
- [ ] Expire element handles on document replacement.
- [ ] Implement `locator.query`.
- [ ] Implement `locator.describe`.
- [ ] Implement `locator.highlight`.
- [ ] Add similar-elements debug suggestions for failures.
- [ ] Add unit tests for locator parsing.
- [ ] Add browser tests for all P0 locator kinds.
- [ ] Add browser tests for ambiguous locator failure.
- [ ] Add browser tests for stale element failure.

### Phase 10 acceptance

- [ ] Role/text/label/placeholder/CSS/XPath locators work against local test pages.
- [ ] Strict mode detects ambiguity.
- [ ] Stale handles fail safely.
- [ ] Locator failures include useful safe debugging hints.

---

## Phase 11 — Action dispatcher and common actions

- [ ] Implement `ActionDispatcher`.
- [ ] Implement actionability checks.
- [ ] Support `trial=true` action checks.
- [ ] Implement `page.click`.
- [ ] Implement `page.double_click`.
- [ ] Implement `page.hover`.
- [ ] Implement `page.fill`.
- [ ] Implement `page.type_text`.
- [ ] Implement `page.press`.
- [ ] Implement `page.check`.
- [ ] Implement `page.uncheck`.
- [ ] Implement `page.select_option`.
- [ ] Implement `page.focus`.
- [ ] Implement `page.clear`.
- [ ] Implement `page.wait_for_selector`.
- [ ] Ensure all actions pass through `PolicyEngine`.
- [ ] Ensure all actions are audited.
- [ ] Detect and report action side effects.
- [ ] Add browser tests for each common action.
- [ ] Add tests for action blocked by mode.
- [ ] Add tests for action blocked by sensitivity.
- [ ] Add tests for timeouts and cancellation.

### Phase 11 acceptance

- [ ] Agent can perform common Playwright-style actions.
- [ ] Actions are auto-waited and policy-gated.
- [ ] Sensitive actions fail closed or require approval.

---

## Phase 12 — Dialog broker

- [ ] Implement `DialogBroker`.
- [ ] Detect JavaScript alert/confirm/prompt dialogs.
- [ ] Implement `dialog.list`.
- [ ] Implement `dialog.accept`.
- [ ] Implement `dialog.dismiss`.
- [ ] Support prompt text for prompt dialogs.
- [ ] Apply policy by page mode and origin.
- [ ] Audit dialog events and decisions.
- [ ] Add browser tests for alert accept.
- [ ] Add browser tests for confirm dismiss.
- [ ] Add browser tests for prompt accept with text.
- [ ] Add tests for sensitive-mode dialog policy.

### Phase 12 acceptance

- [ ] Dialogs are visible to the agent as structured pending objects.
- [ ] Dialog actions are policy-gated and audited.

---

## Phase 13 — Downloads and uploads

- [ ] Implement `DownloadUploadBroker`.
- [ ] Detect downloads created by agent-controlled pages.
- [ ] Implement `download.list`.
- [ ] Implement `download.cancel`.
- [ ] Implement `download.save_as` using approved destination handles only.
- [ ] Add default sandbox download destination option for test profile.
- [ ] Add user approval UI or stub for download destination approval.
- [ ] Implement upload file approval handle model.
- [ ] Implement `upload.set_files` using approved `upload_file_id` handles only.
- [ ] Prevent arbitrary filesystem paths from MCP arguments.
- [ ] Require approval for uploads.
- [ ] Require separate approval for sensitive submission after upload when applicable.
- [ ] Audit all download/upload events.
- [ ] Add browser tests for download list/cancel/save.
- [ ] Add browser tests for upload to file input.
- [ ] Add security tests for arbitrary path rejection.

### Phase 13 acceptance

- [ ] Agent cannot read or write arbitrary filesystem paths.
- [ ] Downloads and uploads require policy approval.
- [ ] All filesystem crossing is audited.

---

## Phase 14 — Trace recorder

- [ ] Implement `TraceRecorder`.
- [ ] Implement `trace.start`.
- [ ] Implement `trace.stop`.
- [ ] Implement `trace.export`.
- [ ] Include action events.
- [ ] Include policy decisions.
- [ ] Include locator resolution attempts.
- [ ] Include navigation events.
- [ ] Include dialog events.
- [ ] Include download/upload events.
- [ ] Include screenshots when enabled.
- [ ] Include redacted accessibility snapshots when enabled.
- [ ] Include console messages when enabled.
- [ ] Include network metadata when enabled.
- [ ] Redact sensitive data before trace persistence.
- [ ] Add trace artifact format documentation.
- [ ] Add tests for trace start/stop/export.
- [ ] Add tests for trace redaction.

### Phase 14 acceptance

- [ ] Agent sessions can be traced and exported.
- [ ] Trace output is useful for replay/debugging without leaking protected data.

---

## Phase 15 — Network observation

- [ ] Implement `NetworkObserver`.
- [ ] Capture request metadata.
- [ ] Capture response metadata.
- [ ] Redact cookies and auth headers.
- [ ] Redact sensitive request bodies.
- [ ] Implement `network.list_requests`.
- [ ] Implement `network.wait_for_request`.
- [ ] Implement `network.wait_for_response`.
- [ ] Add response body retrieval as denied/stub unless P2 is enabled.
- [ ] Add tests for request/response observation.
- [ ] Add tests for header redaction.
- [ ] Add tests for login form request body redaction.

### Phase 15 acceptance

- [ ] Agent can observe safe network metadata.
- [ ] Sensitive headers and bodies are redacted by default.

---

## Phase 16 — Console and page content tools

- [ ] Capture console messages for controlled pages.
- [ ] Implement `console.messages`.
- [ ] Redact console output when sensitivity rules require.
- [ ] Implement `page.frame_tree`.
- [ ] Implement `page.content` behind elevated policy.
- [ ] Ensure hidden fields and sensitive values are redacted or blocked.
- [ ] Implement `page.wait_for_event`.
- [ ] Support events: `download`, `dialog`, `popup`, `navigation`, `load`, `domcontentloaded`, `networkidle`, `request`, `response`, `console`, `crash`, `filechooser`.
- [ ] Add tests for console collection.
- [ ] Add tests for frame tree.
- [ ] Add tests for content redaction.
- [ ] Add tests for wait-for-event cancellation and timeout.

### Phase 16 acceptance

- [ ] Agent can inspect console and frame metadata safely.
- [ ] Full HTML content is unavailable unless policy permits.

---

## Phase 17 — Context emulation and permissions

- [ ] Implement `context.set_viewport`.
- [ ] Implement `context.set_locale`.
- [ ] Implement `context.set_timezone`.
- [ ] Implement `context.set_offline`.
- [ ] Implement `context.get_permissions`.
- [ ] Implement `context.request_permission_grant`.
- [ ] Add policy gates for camera/microphone/location/clipboard/notifications/MIDI/USB/serial/HID/Bluetooth/local-network.
- [ ] Add user approval UI or stub for permission grants.
- [ ] Audit all permission requests and decisions.
- [ ] Add tests for viewport changes.
- [ ] Add tests for locale/timezone if supported in test harness.
- [ ] Add tests for denied high-risk permission grants.

### Phase 17 acceptance

- [ ] Common Playwright-style emulation features work.
- [ ] High-risk permissions cannot be silently granted by the agent.

---

## Phase 18 — Storage state

- [ ] Design storage export format.
- [ ] Implement `context.get_storage_state` with policy redaction.
- [ ] Implement storage import only behind explicit approval or developer profile.
- [ ] Add origin allow/deny filtering.
- [ ] Redact sensitive origins by default.
- [ ] Audit storage import/export.
- [ ] Add tests for storage export from safe origin.
- [ ] Add tests for sensitive origin redaction.
- [ ] Add tests for import denial without approval.

### Phase 18 acceptance

- [ ] Storage state workflows exist for safe test/dev usage.
- [ ] Session secrets are not silently exposed.

---

## Phase 19 — Advanced/debug capabilities

- [ ] Add capability profile system for advanced tools.
- [ ] Implement `page.evaluate_safe` with allowlisted scripts.
- [ ] Define initial safe scripts: document metadata, selection text, redacted form summary, performance summary.
- [ ] Keep raw `page.evaluate` disabled by default.
- [ ] If raw `page.evaluate` is implemented, require developer/debug profile and explicit audit.
- [ ] Implement `network.route` only in debug profile.
- [ ] Implement `network.unroute` only in debug profile.
- [ ] Implement `network.fulfill` only in debug profile.
- [ ] Implement `network.get_response_body` only with policy gates.
- [ ] Implement `context.set_extra_http_headers` only with policy gates.
- [ ] Implement `context.set_geolocation` only with user approval.
- [ ] Add tests proving advanced tools are unavailable in restricted/local default profiles.
- [ ] Add tests for advanced tools in debug profile.

### Phase 19 acceptance

- [ ] Dangerous tools are not available in default profile.
- [ ] Debug tools are explicitly gated, audited, and test-covered.

---

## Phase 20 — User approval UX

- [ ] Design approval prompt surface: modal, infobar, permission chip, side panel, or combination.
- [ ] Implement approval request model.
- [ ] Implement approval response model.
- [ ] Implement pending approval timeout.
- [ ] Implement approval denial handling.
- [ ] Display origin, page title, action summary, and risk class.
- [ ] Show affected file names for upload/download.
- [ ] Show target form/action for sensitive submissions.
- [ ] Prevent spoofing by page content.
- [ ] Audit approval request and response.
- [ ] Add tests for approval required.
- [ ] Add tests for approval denied.
- [ ] Add tests for approval timeout.

### Phase 20 acceptance

- [ ] High-risk actions pause for user approval.
- [ ] Approval UI is browser-controlled and cannot be spoofed by page content.

---

## Phase 21 — Browser UI indicators and controls

- [ ] Add visible per-tab agent-control indicator.
- [ ] Add page mode display.
- [ ] Add sidecar connection indicator.
- [ ] Add current/last action summary.
- [ ] Add emergency stop button.
- [ ] Add locator highlight overlay.
- [ ] Add audit log viewer entry point.
- [ ] Add trace export control.
- [ ] Add user-facing docs/help text for modes.
- [ ] Add browser tests or UI tests for mode display where feasible.

### Phase 21 acceptance

- [ ] User can tell when an agent is connected and active.
- [ ] User can stop agent control immediately.
- [ ] User can inspect recent actions.

---

## Phase 22 — MCP tool parity completion

- [ ] Confirm P0 tool schemas match `docs/CHROMIUM_MCP_AGENT_BROWSER_SPEC.md`.
- [ ] Implement sidecar wrapper for every P0 browser/context/page/locator/action/dialog/audit/trace tool.
- [ ] Implement sidecar wrapper for P1 tools as they land in Chromium.
- [ ] Add generated schema tests for all tools.
- [ ] Add MCP integration tests for common flows.
- [ ] Add examples:
  - [ ] Navigate and click.
  - [ ] Fill a search form.
  - [ ] Wait for navigation.
  - [ ] Handle dialog.
  - [ ] Capture screenshot.
  - [ ] Export trace.
- [ ] Add negative examples showing policy denial.
- [ ] Add tool contract docs with stable request/response examples.

### Phase 22 acceptance

- [ ] MCP clients can call the implemented browser tools reliably.
- [ ] Tool schemas are documented and test-validated.
- [ ] Examples work against local fixtures.

---

## Phase 23 — Test fixture server and end-to-end suite

- [ ] Add deterministic local fixture server.
- [ ] Add simple navigation page.
- [ ] Add search form page.
- [ ] Add login form page.
- [ ] Add password field page.
- [ ] Add payment-like form page.
- [ ] Add account-change form page.
- [ ] Add iframe page.
- [ ] Add popup page.
- [ ] Add dialog page.
- [ ] Add download page.
- [ ] Add upload page.
- [ ] Add network request page.
- [ ] Add console logging page.
- [ ] Add E2E test: navigate and click.
- [ ] Add E2E test: fill/type/press/search.
- [ ] Add E2E test: locator ambiguity.
- [ ] Add E2E test: password redaction.
- [ ] Add E2E test: blocked sensitive form submission.
- [ ] Add E2E test: download approval.
- [ ] Add E2E test: upload approval.
- [ ] Add E2E test: dialog accept/dismiss.
- [ ] Add E2E test: trace export.
- [ ] Add E2E test: audit log completeness.

### Phase 23 acceptance

- [ ] E2E suite covers normal and denied browser-agent flows.
- [ ] Fixtures are deterministic and do not rely on external websites.

---

## Phase 24 — Documentation hardening

- [ ] Expand `docs/SECURITY_MODEL.md`.
- [ ] Expand `docs/TOOL_CONTRACT.md`.
- [ ] Expand `docs/TEST_PLAN.md`.
- [ ] Add `docs/DEVELOPER_SETUP.md`.
- [ ] Add `docs/SIDECAR_PROTOCOL.md`.
- [ ] Add `docs/AUDIT_AND_TRACE_FORMAT.md`.
- [ ] Add `docs/USER_APPROVAL_UX.md`.
- [ ] Add `docs/REBASE_GUIDE.md`.
- [ ] Add `docs/KNOWN_LIMITATIONS.md`.
- [ ] Document all runtime flags.
- [ ] Document supported and unsupported Playwright parity features.
- [ ] Document raw evaluation/network interception restrictions.
- [ ] Document threat model and trust boundaries.
- [ ] Document how to run sidecar tests.
- [ ] Document how to run Chromium browser tests.

### Phase 24 acceptance

- [ ] A new contributor can understand architecture, setup, tool contract, and test strategy from docs.
- [ ] Security restrictions are explicit and not buried in code.

---

## Phase 25 — CI and qualification

- [ ] Add docs lint/check job if repository has CI.
- [ ] Add sidecar lint job.
- [ ] Add sidecar unit test job.
- [ ] Add protocol schema validation job.
- [ ] Add Chromium agent unit test job where feasible.
- [ ] Add browser test job for focused agent tests where feasible.
- [ ] Add E2E fixture suite job where feasible.
- [ ] Add artifact upload for traces on failure.
- [ ] Add failure triage docs.
- [ ] Ensure CI status is documented for each milestone.

### Phase 25 acceptance

- [ ] CI proves docs, schemas, sidecar tests, and available Chromium tests.
- [ ] Failure artifacts are sufficient to debug agent/browser failures.

---

## Phase 26 — Security review checklist

- [ ] Verify no agent path can read password values.
- [ ] Verify no agent path can receive human keystrokes in sensitive/manual modes.
- [ ] Verify clipboard access is blocked or approved in sensitive contexts.
- [ ] Verify arbitrary filesystem paths are rejected for upload/download.
- [ ] Verify renderer-provided sensitivity/origin data is not trusted blindly.
- [ ] Verify every mutating action goes through `PolicyEngine`.
- [ ] Verify raw JavaScript evaluation is unavailable by default.
- [ ] Verify network interception is unavailable by default.
- [ ] Verify audit log cannot be disabled silently during agent session.
- [ ] Verify sidecar authentication is required.
- [ ] Verify sidecar capability is not exposed to web content.
- [ ] Verify page mode state cannot be manipulated by web content.
- [ ] Verify user approval UI cannot be spoofed by web content.
- [ ] Verify traces and audit logs are redacted before persistence.
- [ ] Verify fail-closed behavior for classifier uncertainty.
- [ ] Verify cancellation/emergency stop is reliable.

### Phase 26 acceptance

- [ ] Security review findings are documented.
- [ ] High-risk findings are fixed or explicitly deferred with rationale.

---

## Phase 27 — MVP completion criteria

MVP is complete only when all of the following are true:

- [ ] Chromium builds with `BrowserAgentService` behind a flag.
- [ ] Sidecar MCP server starts and connects to Chromium.
- [ ] Tool schemas for P0 tools are stable and documented.
- [ ] Agent can create/list pages.
- [ ] Agent can navigate and wait for load state.
- [ ] Agent can observe visible elements and accessibility snapshots.
- [ ] Agent can take screenshots when policy allows.
- [ ] Agent can click/fill/type/press/select/check/uncheck.
- [ ] Role/text/label/placeholder/CSS/XPath locators work.
- [ ] Manual mode blocks mutating actions.
- [ ] Sensitive mode blocks observation and mutating actions.
- [ ] Password field values are never exposed.
- [ ] Downloads/uploads require approved handles or user approval.
- [ ] Dialog list/accept/dismiss works.
- [ ] Audit log records every tool call and policy decision.
- [ ] Trace start/stop/export works.
- [ ] Local fixture E2E suite passes.
- [ ] Security model docs are current.
- [ ] Tool contract docs are current.
- [ ] Test plan docs are current.

---

## Phase 28 — Post-MVP Playwright parity expansion

- [ ] Add `page.drag_and_drop`.
- [ ] Add HAR recording if needed.
- [ ] Add video recording if needed.
- [ ] Add richer network response observation.
- [ ] Add storage-state import/export workflows.
- [ ] Add geolocation emulation with approval.
- [ ] Add extra HTTP headers with policy gate.
- [ ] Add safe evaluation script registry.
- [ ] Add debug-only raw evaluation, if still justified.
- [ ] Add debug-only network routing/interception, if still justified.
- [ ] Add locator suggestion tool.
- [ ] Add natural-language-to-locator helper.
- [ ] Add richer browser UI panel for agent state and audit log.

---

## Phase 29 — Long-term maintainability

- [ ] Minimize Chromium patch surface.
- [ ] Keep agent code under clearly named directories.
- [ ] Avoid modifying Blink unless needed.
- [ ] Avoid changing broad browser input paths unless necessary.
- [ ] Maintain a rebase guide with common conflict areas.
- [ ] Track upstream Chromium changes affecting WebContents, accessibility, permissions, downloads, and input dispatch.
- [ ] Add owners/reviewers guidance for security-sensitive files.
- [ ] Keep protocol changes versioned.
- [ ] Maintain backwards-compatible sidecar tool schemas where possible.
- [ ] Add migration notes for breaking tool changes.

---

## Initial vertical slice recommendation

Implement in this order for fastest useful proof:

1. Browser service skeleton behind flag.
2. Sidecar connection and `browser.version`.
3. Page registry and `page.list` / `page.get_info`.
4. `page.goto` / `page.wait_for_load_state`.
5. Accessibility snapshot and visible element list.
6. Role/text/label locator resolver.
7. `page.click` / `page.fill` / `page.type_text`.
8. Page modes and basic policy enforcement.
9. Audit log.
10. Trace start/stop/export.
11. Dialog handling.
12. Download/upload approval stubs.
13. Local fixture E2E suite.

This slice validates architecture, tool shape, browser-process enforcement, and MCP integration before spending time on advanced network interception or raw evaluation.
