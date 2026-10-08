# Chromium MCP Agent Browser Specification

Status: Active implementation
Repository: `ekkus93/chromium-mcp`
Primary goal: host an MCP server inside Chromium and let an authenticated local MCP client control Chromium through native browser APIs.

## 1. Design goals

- Run MCP in Chromium's browser process; do not use a separate MCP sidecar.
- Call Chromium browser APIs directly. CDP, remote debugging, Playwright, and DevTools protocol adapters are not control paths.
- Expose broad browser control: profiles and contexts, tabs, frames, navigation, DOM and JavaScript, accessibility, input, dialogs, downloads/uploads, permissions, storage, network, console, screenshots, and emulation.
- Do not add an agent-specific policy allowlist, page modes, sensitive-data redaction, approval gates, or feature tiers that restrict the local caller's browser operations.
- Keep the service explicitly opt-in and local. Preserve Chromium's existing sandbox, profile ownership, platform permission prompts, and operating-system access controls.
- Keep the implementation under `chrome/browser/agent/` and rebaseable against upstream Chromium.

## 2. Architecture

```text
MCP client
    | MCP Streamable HTTP over loopback
    v
Chromium browser process
  BrowserAgentService
    +-- MCP request/response handling
    +-- Context and page registries
    +-- Native browser API adapters
    +-- Optional audit and trace output
    +-- WebContents / RenderFrameHost / input / downloads / permissions
```

The browser process hosts the MCP endpoint and owns all tool execution. The service calls native Chromium APIs such as `Browser`, `Profile`, `WebContents`, `RenderFrameHost`, navigation controllers, download managers, and permission services. It does not forward calls through CDP or a second browser-control protocol.

## 3. MCP transport

Implement the current MCP Streamable HTTP transport on an explicitly enabled loopback endpoint. The endpoint must reject web-origin requests and authenticate local MCP clients. Transport authentication protects the endpoint from unrelated callers; it must not filter which browser tools the authenticated client can use. Keep the MCP wire format compatible with the published MCP specification and test it with a standard MCP client.

## 4. Capability surface

Implement tools that map to native browser operations, including direct JavaScript evaluation and access to browser data available to the profile. Do not redact values, block actions based on page sensitivity, require browser-agent approvals, or hide tools behind profile modes. Chromium's normal UI and platform permission handling still apply where native Chromium APIs require them.

Tool schemas should describe valid arguments and results, not impose policy limits. Errors should report native API failures, invalid arguments, unavailable objects, timeouts, or operating-system denials clearly.

## 5. Security and privacy boundaries

The service is disabled by default. It listens only on loopback and validates HTTP `Origin` and local-client authentication to prevent webpages or unrelated local processes from using the endpoint. Never expose the MCP endpoint as CDP or share CDP debugging ports. MCP access is equivalent to direct access to the selected Chromium profile; users should run it only when they intend to grant that access.

Optional audit and trace output may help diagnose use but must not deny, change, or redact operations. Normal browser security boundaries remain in effect.

## 6. Initial implementation path

1. Keep the profile-scoped service behind a feature and runtime opt-in.
2. Embed MCP request handling and loopback Streamable HTTP in the browser process.
3. Implement `browser.version`, `browser.list_contexts`, `page.list`, and `page.get_info` using native Chromium objects.
4. Add navigation, evaluation, observation, and input tools.
5. Expand coverage to browser permissions, downloads/uploads, storage, network, and dialogs.
6. Validate compatibility with MCP clients and browser tests.

## 7. Qualification

The MVP is complete when Chromium builds with the opt-in MCP service, an MCP client connects directly to Chromium, core page/context tools work through native APIs without CDP, and browser tests cover lifecycle, direct evaluation, input, navigation, downloads, permissions, and transport isolation. Documentation must state that authenticated clients receive broad control of the selected profile.
