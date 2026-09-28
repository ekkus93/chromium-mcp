# Chromium MCP Agent Browser Security Model

Status: Phase 0 stub  
Companion spec: `docs/CHROMIUM_MCP_AGENT_BROWSER_SPEC.md`  
Canonical TODO: `docs/CHROMIUM_MCP_AGENT_BROWSER_TODO.md`

## Security objective

The browser-agent system must let an agent operate Chromium only through
explicit, policy-gated browser capabilities. The agent must not gain raw browser
privilege, raw filesystem access, raw JavaScript execution, raw CDP access, raw
Mojo access, or access to sensitive user data by default.

## Trust boundaries

- Browser process: trusted enforcement point for page mode, permissions,
  downloads/uploads, audit, trace, and redaction decisions.
- Renderer processes: untrusted. Renderer-provided DOM, accessibility labels,
  origin-like strings, file paths, and sensitivity hints must be validated before
  being used in privileged browser decisions.
- MCP sidecar: local protocol adapter, not the policy authority. It validates
  schemas and forwards requests, but Chromium must independently enforce policy.
- Agent client: untrusted caller. Treat prompts, tool arguments, and proposed
  actions as potentially hostile or confused.
- Web content: adversarial input. Pages can prompt-inject the agent, spoof UI
  inside content, mutate DOM after observation, and attempt to exfiltrate data.
- User: final authority for sensitive operations and emergency stop.

## Default deny surfaces

The default profile must not expose:

- raw CDP passthrough;
- raw Mojo passthrough;
- raw arbitrary JavaScript evaluation;
- raw network interception or response fulfillment;
- arbitrary upload/download filesystem paths;
- password, OTP, payment, authentication, or account-change field values;
- human keystrokes in manual or sensitive contexts;
- silent permission grants for camera, microphone, location, clipboard,
  notifications, MIDI, USB, serial, HID, Bluetooth, or local-network access.

## Page modes

The initial policy model uses page-level modes:

- `MANUAL`: agent observation is limited to safe metadata; mutating actions are
  blocked.
- `ASSISTED`: low-risk actions may be allowed with audit; high-risk actions need
  user approval.
- `AGENT`: autonomous low-risk actions may be allowed with audit and policy
  checks.
- `SENSITIVE`: observation and mutation are minimized; sensitive fields and
  flows fail closed.

## Sensitive data handling

Sensitive values must be redacted before leaving the browser process and before
persistence to audit or trace outputs. If the classifier is uncertain, the safe
fallback is protected/sensitive.

Initial sensitive classes:

- password fields;
- OTP, MFA, auth-code, and recovery-code fields;
- payment and card fields;
- login, OAuth, consent, and account-change submissions;
- file paths outside approved handles;
- cookies, auth headers, CSRF tokens, and session-bearing storage where exposed
  by future storage/network tools.

## Required audit behavior

Every MCP tool call must produce an audit record, including denied calls. Audit
records must include the policy decision and rule identifiers, but must not store
unredacted sensitive values.

## Fail-closed requirements

- Unknown page mode: block mutating action.
- Unknown or stale page/frame/element handle: block action.
- Ambiguous sensitive classification: protect as sensitive.
- Missing sidecar authentication: reject connection.
- Missing approval for high-risk operation: block or wait for approval, then
  timeout safely.
- Emergency stop: cancel pending actions and block new mutating actions until
  cleared by the user.

## Phase 0 note

This file is a security-model stub. Later phases must expand it with concrete
policy matrices, rule identifiers, threat scenarios, browser test references,
and security review outcomes.
