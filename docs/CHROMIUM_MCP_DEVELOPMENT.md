# Chromium MCP Agent Browser: Development and Use

This guide covers Linux development, local validation, starting the in-process
MCP server, connecting local MCP clients, and packaging status. The MCP feature
is experimental and disabled by default. Current qualification is Linux-only.

## Get the source and prepare a build

This is a Chromium source checkout, so use Chromium's
[checkout instructions](get_the_code.md) and install the documented Linux build
dependencies. The [Linux build guide](linux/build_instructions.md) covers host
setup and common build problems.

From the source root, generate a build directory and configure it with GN:

```sh
gn gen out/Default
gn args out/Default
```

For a lower-cost local build, set these values in the GN args editor:

```gn
is_debug = false
is_component_build = true
symbol_level = 0
```

Build the browser and agent unit-test binary:

```sh
autoninja -C out/Default chrome
autoninja -C out/Default unit_tests
```

Chromium's checkout and build instructions are authoritative for required
system packages and supported build options. Do not commit `out/`, generated
files, local GN args, credentials, or traces.

## Formatting, checks, and tests

There is no single lint command configured for this MCP feature. Run the
Chromium C++ formatting check on changed C++ files and check whitespace:

```sh
git clang-format --diff HEAD -- chrome/browser/agent chrome/common/agent
git diff --check
```

The `agent_protocol` package has Node schema tests and a browser integration
suite. Run them from the package directory:

```sh
cd agent_protocol
npm ci
npm test
CHROME_BIN=../out/Default/chrome npm run test:chromium
```

The browser integration suite expects a built Chromium binary and launches it
with a temporary profile. An optional independently maintained MCP Python SDK
check is enabled by setting `CHROMIUM_MCP_SDK_PYTHONPATH` to the SDK's Python
package directory.

Run the focused Chromium agent unit tests from the source root:

```sh
out/Default/unit_tests \
  --gtest_filter='BrowserAgent*.*'
```

The broader [Test Plan](TEST_PLAN.md) describes additional browser coverage and
environment-specific cases.

## Start the MCP server

Use a dedicated user-data directory so this test instance does not attach to an
existing browser profile. Chromium creates the directory on first launch if it
does not already exist. Choose a fixed port for client configuration; this
example uses `17373`:

```sh
MCP_PROFILE="$HOME/.local/share/chromium-mcp-profile"
out/Default/chrome \
  --user-data-dir="$MCP_PROFILE" \
  --enable-features=BrowserAgentService \
  --enable-browser-agent-service \
  --browser-agent-mcp-port=17373
```

The service listens on IPv4 loopback. Keep the client on the same machine and
use `127.0.0.1` in its URL. Do not expose the endpoint through a public
interface, tunnel, or reverse proxy. An authenticated client can control the
browser profile and use the full tool surface.

For the default Linux profile, the service writes these files after startup:

```text
$MCP_PROFILE/Default/BrowserAgentRuntime/browser-agent-mcp-endpoint
$MCP_PROFILE/Default/BrowserAgentRuntime/browser-agent-mcp-token
```

The token file is created with owner-only POSIX permissions. Read its value
into the environment without printing or committing it:

```sh
MCP_RUNTIME="$MCP_PROFILE/Default/BrowserAgentRuntime"
export CHROMIUM_MCP_TOKEN="$(cat "$MCP_RUNTIME/browser-agent-mcp-token")"
export CHROMIUM_MCP_ENDPOINT="$(cat "$MCP_RUNTIME/browser-agent-mcp-endpoint")"
```

The endpoint URL is `http://127.0.0.1:17373/mcp`. If you choose a different
port, use the URL written to the endpoint file. Port `0` asks the OS for an
available port, but a client configuration must then be updated whenever the
selected port changes.

To smoke-test the endpoint with `curl`:

```sh
curl --fail-with-body "$CHROMIUM_MCP_ENDPOINT" \
  -H "Authorization: Bearer $CHROMIUM_MCP_TOKEN" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: server/discover' \
  --data-binary @- <<'JSON'
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "server/discover",
  "params": {
    "_meta": {
      "io.modelcontextprotocol/protocolVersion": "2026-07-28",
      "io.modelcontextprotocol/clientCapabilities": {},
      "io.modelcontextprotocol/clientInfo": {
        "name": "curl",
        "version": "1"
      }
    }
  }
}
JSON
```

See the [Tool Contract](TOOL_CONTRACT.md) for the MCP wire contract and the
complete tool catalog.

## Connect Codex

Codex CLI and its IDE extension read the same MCP configuration. Export
`CHROMIUM_MCP_TOKEN` in the environment before starting Codex, then add this
server entry to `~/.codex/config.toml`:

```toml
[mcp_servers.chromium_agent_browser]
url = "http://127.0.0.1:17373/mcp"
bearer_token_env_var = "CHROMIUM_MCP_TOKEN"
```

Run `codex mcp list` or `/mcp` in the Codex TUI to check its connection. For
example, ask Codex: “Call `browser.version` and summarize the Chromium build
metadata.” See the official [Codex MCP guide](https://developers.openai.com/codex/mcp)
for current configuration options.

## Connect Claude Code

Claude Code supports Streamable HTTP MCP servers. Create a local `.mcp.json`
configuration in the project where you run Claude Code:

```json
{
  "mcpServers": {
    "chromium-agent-browser": {
      "type": "http",
      "url": "http://127.0.0.1:17373/mcp",
      "headers": {
        "Authorization": "Bearer ${CHROMIUM_MCP_TOKEN}"
      }
    }
  }
}
```

Export `CHROMIUM_MCP_TOKEN` before starting Claude Code. Keep the token out of
checked-in configuration; the variable reference above is safe to share, but
the resolved token is not. Run `claude mcp list`, then `/mcp` inside Claude
Code to verify the connection. For example, ask Claude Code: “Call
`browser.version` and tell me which Chromium build is running.” See the
official [Claude Code MCP guide](https://code.claude.com/docs/en/mcp) for
current setup and authentication details.

## Packaging

This repository does not currently maintain an AppImage build or release
recipe. The supported local development artifact documented here is the
Chromium binary at `out/Default/chrome`; it is not a packaged AppImage.
Distribution packaging needs a separately maintained recipe and validation
for the target Linux distributions. For instructions to use a locally built
AppImage with MCP and a client, see the
[AppImage and Codex guide](CHROMIUM_MCP_APPIMAGE_CODEX.md) or the
[AppImage and Claude Code guide](CHROMIUM_MCP_APPIMAGE_CLAUDE_CODE.md).
