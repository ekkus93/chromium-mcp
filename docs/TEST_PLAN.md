# Chromium MCP Agent Browser Test Plan

Status: Phase 0 stub  
Companion spec: `docs/CHROMIUM_MCP_AGENT_BROWSER_SPEC.md`  
Canonical TODO: `docs/CHROMIUM_MCP_AGENT_BROWSER_TODO.md`

## Test strategy

The project should validate behavior at the narrowest layer that can prove the
claim, then add browser/end-to-end coverage for cross-boundary behavior.

Planned layers:

1. Documentation checks.
2. Protocol schema validation.
3. MCP sidecar unit tests.
4. Sidecar integration tests against a fake browser-agent service.
5. Chromium C++ unit tests for policy, redaction, audit, and handle logic.
6. Chromium browser tests for tabs, navigation, WebContents integration,
   permissions, dialogs, downloads/uploads, and UI approvals.
7. Deterministic local fixture E2E tests for full agent flows.

## Local Chromium build entry points

No repository-specific GN args are committed yet. Use normal Chromium setup and
configure a local output directory.

Common Linux commands:

```sh
gn gen out/Default
gn args out/Default
autoninja -C out/Default chrome
```

Initial broad test targets:

```sh
autoninja -C out/Default unit_tests
autoninja -C out/Default browser_tests
out/Default/unit_tests --gtest_filter='*Agent*'
out/Default/browser_tests --gtest_filter='*Agent*'
```

The `*Agent*` filters are forward-looking placeholders until Phase 1 creates the
first agent test suites.

## Future sidecar test entry points

Phase 3 should add `mcp-sidecar/` with package-level commands such as:

```sh
npm test
npm run lint
npm run build
```

or equivalent commands if the sidecar implementation language is not TypeScript.
The implementation language decision must update this file.

## Required fixture coverage

The Phase 23 fixture server should include deterministic local pages for:

- simple navigation;
- search form;
- login form;
- password field;
- payment-like form;
- account-change form;
- iframe content;
- popup/new-window;
- JavaScript dialogs;
- downloads;
- uploads;
- network requests;
- console logging.

## Core acceptance tests by feature area

- Page modes: `MANUAL` blocks mutating actions; `SENSITIVE` blocks observation
  and mutation beyond minimal safe metadata.
- Policy engine: every mutating action passes through policy and records the
  applied rules.
- Sensitive classifier: password values, OTP fields, payment fields, login
  submissions, and account-change submissions fail closed or require approval.
- Locators: role, label, text, placeholder, CSS, and XPath locators resolve on
  fixture pages; ambiguous locators fail safely with safe suggestions.
- Actions: click, fill, type, press, select, check, and uncheck operate through
  browser-mediated input paths, not raw DOM mutation.
- Audit: allowed and denied calls are logged; redaction happens before
  persistence/export.
- Trace: traces include action, policy, navigation, dialog, download/upload, and
  optional screenshot/accessibility events without leaking sensitive values.
- Filesystem crossing: uploads and downloads use approved handles only; raw
  filesystem paths from MCP arguments are rejected.
- Sidecar authentication: unauthenticated local clients are rejected.

## Current CI capacity

At Phase 0, `.github/workflows/` contains only `close-pull-request.yml`. There
is no active validation workflow for docs, sidecar tests, protocol schemas,
Chromium unit tests, browser tests, or E2E fixtures yet.

Phase 25 must add CI jobs for the implemented layers. Until that lands, every
commit should document local validation status in the commit message or follow-up
handoff notes.

## Phase 0 validation

This documentation-only Phase 0 slice is validated by repository inspection and
manual review. No automated CI check currently proves it.
