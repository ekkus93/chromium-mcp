# Embedded MCP Protocol

Chromium hosts the MCP server in the browser process. The server accepts MCP
JSON-RPC over local Streamable HTTP and dispatches tools to Chromium-native
browser APIs. No sidecar, CDP, DevTools protocol, or custom browser-control
protocol is used.

The message schema covers MCP discovery, tool listing, tool calls, and JSON-RPC
responses. The implementation targets the current published MCP revision,
`2026-07-28`; see the [MCP specification](https://modelcontextprotocol.io/specification/2026-07-28).

Run the schema tests with `npm ci && npm test` from this directory. The HTTP
endpoint's local authentication protects access to the browser process. It does
not restrict the authenticated caller's available browser tools.
