# Use the Chromium MCP AppImage with Codex

This guide starts the MCP server in the Chromium MCP AppImage and connects
Codex to it on the same Linux machine.

## Find the AppImage

In the checkout where the AppImage was built, it is located at:

```text
out/Default/Chromium-MCP-x86_64.AppImage
```

The `out/` directory is ignored by Git. The AppImage is a local build artifact,
so it is not included in a fresh checkout or in the files on GitHub. If yours is
in a different location, use that path for `APPIMAGE` below. If needed, make it
executable with `chmod +x /path/to/Chromium-MCP-x86_64.AppImage`.

## Start Chromium with MCP enabled

Opening the AppImage normally starts Chromium with MCP disabled. To start the
MCP server, launch it from a terminal with the feature and runtime switches.
This uses a dedicated profile, which Chromium creates on first launch:

```sh
APPIMAGE="$PWD/out/Default/Chromium-MCP-x86_64.AppImage"
MCP_PROFILE="$HOME/.local/share/chromium-mcp-profile"
"$APPIMAGE" \
  --user-data-dir="$MCP_PROFILE" \
  --enable-features=BrowserAgentService \
  --enable-browser-agent-service \
  --browser-agent-mcp-port=17373
```

Keep this Chromium process running while Codex uses the MCP server. The
endpoint listens on IPv4 loopback at `127.0.0.1`, port `17373`. Keep Codex on
the same machine and do not expose the endpoint through a public interface,
tunnel, or reverse proxy.

If you already opened the AppImage normally, that existing browser process
does not have MCP enabled. Run the command above to open a browser with MCP
enabled; it uses the separate `chromium-mcp-profile` profile.

### Ubuntu 24.04 and AppArmor

On Ubuntu 24.04, AppArmor may block the unprivileged user namespaces Chromium
uses for its Linux sandbox. Chromium then exits with `No usable sandbox` before
the MCP server starts. Do not add `--no-sandbox`; run the helper below from a
terminal inside your graphical desktop session instead. It extracts the
AppImage under `~/.local/opt/chromium-mcp`, installs an AppArmor profile that
allows the required user namespaces for that extracted Chromium binary, and
starts it with the MCP switches above. `sudo` is needed to install the
system-wide AppArmor profile.

From the repository checkout:

```sh
./tools/chromium-mcp-appimage.sh
```

Or pass the AppImage path explicitly:

```sh
./tools/chromium-mcp-appimage.sh /path/to/Chromium-MCP-x86_64.AppImage
```

The script checks for a graphical display and refreshes the extraction when
the AppImage changes. Leave the terminal running while Codex connects. If the
script reports that no graphical display is available, open a terminal in the
Ubuntu desktop session and run it there; a remote shell without the desktop's
display authorization cannot start the browser window.

## Load the endpoint token

After the browser starts, open a second terminal. Read the endpoint and token
from the profile runtime directory. This command exports the token without
printing it:

```sh
MCP_RUNTIME="$HOME/.local/share/chromium-mcp-profile/Default/BrowserAgentRuntime"
export CHROMIUM_MCP_TOKEN="$(cat "$MCP_RUNTIME/browser-agent-mcp-token")"
CHROMIUM_MCP_ENDPOINT="$(cat "$MCP_RUNTIME/browser-agent-mcp-endpoint")"
printf 'MCP endpoint: %s\n' "$CHROMIUM_MCP_ENDPOINT"
```

The token is a credential. Do not paste it into source files, check it into
Git, or share it. Keep the terminal with the exported variable open and start
Codex from that same terminal so it inherits the token.

Codex CLI may connect through a shared app-server daemon that was started
before this token was exported. In that case, starting `codex` from this
terminal can reuse the old daemon, and `/mcp` may show
`chromium_agent_browser` with **0 tools**. Save any active Codex work, then
restart the daemon from this terminal so it inherits the token:

```sh
codex app-server daemon stop
codex app-server daemon start
codex
```

Stopping the daemon disconnects its active Codex sessions. After it starts,
check `/mcp` again; the Chromium entry should list its tools. `codex mcp list`
only confirms that the server is configured, so use `/mcp` to confirm that
Codex loaded the tools.

## Configure Codex

Add this entry to `~/.codex/config.toml`:

```toml
[mcp_servers.chromium_agent_browser]
url = "http://127.0.0.1:17373/mcp"
bearer_token_env_var = "CHROMIUM_MCP_TOKEN"
```

Start Codex from the terminal where `CHROMIUM_MCP_TOKEN` is exported. Check
the connection with:

```sh
codex mcp list
```

In the Codex TUI, `/mcp` also shows the connection. To verify that the browser
tools work, ask Codex to call `browser.version`.

If you selected a different port when starting Chromium, change the URL in
`config.toml` to match the endpoint URL written in
`browser-agent-mcp-endpoint`.

## Troubleshooting

- **Codex cannot connect:** Confirm the MCP-enabled browser process is still
  running, then check that the URL uses `127.0.0.1` and the same port as the
  endpoint file.
- **Token file is missing:** The server writes runtime files after startup.
  Check that both `--enable-features=BrowserAgentService` and
  `--enable-browser-agent-service` were passed to the AppImage.
- **Codex reports a missing token environment variable:** Start Codex from the
  terminal where `CHROMIUM_MCP_TOKEN` was exported.
- **Codex shows `chromium_agent_browser` with 0 tools:** The shared app-server
  daemon may have started before the token was exported. Follow the daemon
  restart steps above, then check `/mcp` again.
- **The browser opened without MCP:** Start another instance with the command
  in this guide. MCP must be enabled when Chromium starts.
- **`No usable sandbox` on Ubuntu 24.04:** Use the helper in the Ubuntu
  AppArmor section above. Do not use `--no-sandbox` for normal browsing.

For the protocol details and complete tool catalog, see the
[Tool Contract](TOOL_CONTRACT.md). For setup using the built `chrome` binary,
Claude Code instructions, tests, and other development commands, see the
[Chromium MCP Development and Use Guide](CHROMIUM_MCP_DEVELOPMENT.md).
