# Chromium MCP Agent Browser Security Model

Status: Opt-in in-process MCP endpoint and initial native tab tools implemented
Companion spec: `docs/CHROMIUM_MCP_AGENT_BROWSER_SPEC.md`  
Canonical TODO: `docs/CHROMIUM_MCP_AGENT_BROWSER_TODO.md`

## Design objective

The MCP server runs inside Chromium and calls browser-native APIs directly. The
project does not use CDP as a control channel, a separate MCP sidecar, or an
agent-specific policy allowlist. The MCP tool surface should make the browser's
native capabilities available to the local caller.

Normal Chromium and operating-system security boundaries remain authoritative:
profile ownership and locks, sandboxing, OS permissions, and browser permission
prompts behave as they do for ordinary browser use. The MCP server must not
invent additional restrictions based on page mode, sensitivity heuristics, or
approval classes.

## Trust boundaries

- Browser process: hosts MCP and invokes the native browser APIs.
- Renderer processes and web content: remain untrusted and cannot access the
  MCP endpoint or its local credentials.
- MCP client: local caller authenticated with the profile's bearer token; tool
  arguments are still validated before use.
- Operating system: controls process identity, files, device permissions, and
  sandbox restrictions.

## Endpoint requirements

The endpoint must be local, opt-in, and unavailable to web origins. Its
transport authentication protects the browser process from unrelated local
clients; it is not a tool-level permission system. Do not expose CDP, raw Mojo,
or a second remote-control port as part of this service.

The service may record actions when the user configures an audit path, but
logging must not block or rewrite browser operations. The service must not
redact browser data from the authenticated local caller unless Chromium's native
API itself withholds that data.

## Implemented startup gate

The service is disabled unless both the disabled-by-default
`BrowserAgentService` feature and `--enable-browser-agent-service` are set.
Pass `--browser-agent-mcp-port=0` to select an ephemeral loopback port. The
service writes its per-profile token to
`<profile>/BrowserAgentRuntime/browser-agent-mcp-token` with owner-only POSIX
permissions and writes the selected endpoint to the adjacent
`browser-agent-mcp-endpoint` file. It currently exposes metadata and initial tab
controls; broader browser controls remain in the project TODO.
