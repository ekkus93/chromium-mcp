# Chromium MCP Agent Browser Tool Contract

Status: Phase 0 stub  
Companion spec: `docs/CHROMIUM_MCP_AGENT_BROWSER_SPEC.md`  
Canonical TODO: `docs/CHROMIUM_MCP_AGENT_BROWSER_TODO.md`

## Contract purpose

This document defines the stable shape expected from the browser-agent protocol
and MCP sidecar. It starts as a Phase 0 stub and will become the normative
request/response/schema reference as implementation lands.

## Layering

```text
Agent host / Claude Code / MCP client
        |
        | MCP tools, resources, prompts
        v
Local MCP sidecar
        |
        | browser-agent protocol, authenticated local transport
        v
Chromium browser process BrowserAgentService
        |
        | Chromium-native policy, observation, input, audit, trace
        v
Pages, frames, WebContents, permissions, downloads, dialogs
```

The MCP sidecar is not the policy authority. Chromium must revalidate every
request before observing or mutating browser state.

## Handle formats

Handles are opaque, short, and scoped to a browser-agent session. They must not
encode privileged data.

Initial handle kinds:

- `context_id`
- `page_id`
- `frame_id`
- `element_id`
- `action_id`
- `dialog_id`
- `download_id`
- `trace_id`

Element handles are short-lived and must expire on navigation or document
replacement.

## Request envelope

Every browser-agent request should carry:

```json
{
  "protocol_version": "0.1",
  "request_id": "req_...",
  "tool": "page.click",
  "arguments": {},
  "reason": "short human-readable caller reason",
  "timeout_ms": 5000
}
```

## Response envelope

Every response should be structured:

```json
{
  "ok": true,
  "request_id": "req_...",
  "action_id": "act_...",
  "policy": {
    "decision": "allowed",
    "rules_applied": ["page_mode_agent", "non_sensitive_element"]
  },
  "result": {},
  "page_state": {
    "page_id": "p_...",
    "url": "https://example.test/",
    "title": "Example",
    "load_state": "idle"
  }
}
```

Failures should not require string parsing:

```json
{
  "ok": false,
  "request_id": "req_...",
  "policy": {
    "decision": "blocked",
    "rules_applied": ["page_mode_manual"]
  },
  "error": {
    "code": "policy_blocked",
    "message": "Mutating actions are blocked in MANUAL mode.",
    "retryable": false
  }
}
```

## LocatorSpec

The tool contract follows Playwright-style locators but serializes them as data:

```json
{
  "kind": "role",
  "role": "button",
  "name": "Continue",
  "exact": false
}
```

Planned locator kinds:

- `role`
- `label`
- `text`
- `placeholder`
- `alt_text`
- `title`
- `test_id`
- `css`
- `xpath`
- `nth`
- `chain`

Accessibility-oriented locators should be preferred over CSS/XPath in agent
examples.

## P0 MCP tools

Initial P0 tool families:

- browser/context: `browser.version`, `browser.capabilities`,
  `browser.list_contexts`, `context.new`, `context.close`,
  `context.set_default_timeout`;
- page: `page.list`, `page.new`, `page.close`, `page.bring_to_front`,
  `page.get_info`, `page.set_mode`, `page.goto`, `page.reload`,
  `page.go_back`, `page.go_forward`, `page.wait_for_load_state`,
  `page.wait_for_url`;
- observation: `page.snapshot_accessibility`, `page.visible_elements`,
  `page.text_content`, `page.screenshot`;
- locator: `locator.query`, `locator.describe`, `locator.highlight`;
- actions: `page.click`, `page.double_click`, `page.hover`, `page.fill`,
  `page.type_text`, `page.press`, `page.check`, `page.uncheck`,
  `page.select_option`, `page.focus`, `page.clear`,
  `page.wait_for_selector`;
- dialogs: `dialog.list`, `dialog.accept`, `dialog.dismiss`;
- audit/trace: `audit.list_actions`, `audit.get_action`, `trace.start`,
  `trace.stop`, `trace.export`.

## Policy-visible fields

Action tools should include enough metadata for audit and policy decisions:

- `page_id` or `context_id`;
- locator or handle;
- timeout;
- `trial` where supported;
- caller reason;
- user-visible flag;
- explicit file/download/upload handles for filesystem-crossing operations.

## Phase 0 note

This contract is intentionally not a complete schema. Phase 2 and Phase 3 must
replace this stub with checked protocol schemas and sidecar validation tests.
