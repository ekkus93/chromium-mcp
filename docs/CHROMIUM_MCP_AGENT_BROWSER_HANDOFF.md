# Chromium MCP Agent Browser Handoff

Historical snapshot: this handoff predates the current source revision and
contains obsolete build and event-stream validation statements. Use the
canonical [TODO](CHROMIUM_MCP_AGENT_BROWSER_TODO.md) and
[remediation checklist](CHROMIUM_MCP_AGENT_BROWSER_REMEDIATION_TODO.md) for
current status. The dated `CHROMIUM_MCP_AGENT_BROWSER_HANDOFF_2026-10-09.md`
is retained as historical evidence.

Updated: 2026-10-08

## Project State

The project embeds an opt-in MCP endpoint in Chromium’s browser process. Browser tools use Chromium-native APIs; CDP, DevTools remote debugging, Playwright, and a sidecar are not control paths. The implementation and canonical checklist are in `chrome/browser/agent/` and `docs/CHROMIUM_MCP_AGENT_BROWSER_TODO.md`.

The worktree is on branch `master`, based on `044745067c` (`Document Chromium MCP Phase 0 scaffolding`). Changes are **not committed or staged**. Before this handoff document, Git showed 20 modified tracked files plus four untracked paths: `.serena/`, `agent_protocol/`, `chrome/browser/agent/`, and `content/public/browser/browser_automation.h`. This document is an additional new file under `docs/`.

## Build Status

The user stopped the `chrome` build before moving computers. It completed 245 of 2,306 compile steps without a compile failure; the remaining 2,061 were interrupted. The build had been pinned to CPU 0, limited to one Ninja job, and run at low priority. The current `out/Default/chrome` is still the older binary. Resume the build on the destination computer after configuring its own Chromium checkout and output directory.

Resume with one CPU core and one Ninja job:

```sh
nice -n 19 taskset -c 0 env PATH=/home/phil/depot_tools:$PATH \
  autoninja --fast_local -j1 -C out/Default chrome
```

The current GN args are `is_debug = false`, `is_component_build = true`, and `symbol_level = 0`. Check free disk space before resuming; the latest reading was 30 GB.

## Verification

Passed: C++ `clang-format --dry-run --Werror`, `git diff --check`, Python syntax compilation for `agent_protocol/tests/chromium_mcp_integration.py`, and `npm test --prefix agent_protocol` (2 tests). `gn check out/Default //chrome/browser/agent:agent` passed earlier. The latest C++ changes have not yet completed a Chromium build. The MCP/browser integration suite and Chromium unit tests must be rerun after the rebuilt browser is ready.

## Remaining TODOs

- Validate SSE notifications for tab create/close, popups, and navigation, and validate subresource request/response headers and bodies in the browser integration suite. Assertions are already present.
- Rebuild `chrome`, then rerun the MCP/browser integration suite and relevant Chromium unit tests; update checklist status only from results.
- Verify a real OS/device permission denial on a platform with controllable OS permission settings. This Ubuntu 24.04 TTY session has no such control available; record platform and evidence when tested elsewhere.

GitHub Actions setup is intentionally excluded. For the computer move, preserve the full uncommitted worktree, including untracked paths; `out/Default` is a local build output and should be regenerated on the destination machine.
