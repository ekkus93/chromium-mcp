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

## Connect Codex

After Chromium starts, open a second terminal in the repository checkout and
run:

```sh
./tools/chromium-mcp-codex.sh
```

The launcher reads the endpoint and token from the browser profile, configures
the Codex MCP entry, exports the token without displaying it, and starts
Codex. It also restarts an existing Codex app-server daemon so the new process
receives a changed token or MCP endpoint. Restarting the daemon disconnects any
Codex sessions using it; rerunning the launcher with the same token preserves
them. In the Codex TUI, run `/mcp`; the Chromium entry should list its tools. To
verify the browser connection, ask Codex to call `browser.version`.

The default runtime directory is
`~/.local/share/chromium-mcp-profile/Default/BrowserAgentRuntime`. Set
`CHROMIUM_MCP_RUNTIME_DIR` before running the launcher if you chose a different
Chromium profile. The token is a credential: do not paste it into source files,
check it into Git, or share it.

The setup can also be run separately with
`./tools/chromium-mcp-codex.sh setup`; the normal launcher configures Codex
automatically, so this is optional. The script uses Codex's `mcp` CLI and
updates only the `chromium_agent_browser` entry if its endpoint changed.

## Troubleshooting

- **Codex cannot connect:** Confirm the MCP-enabled browser process is still
  running, then rerun `./tools/chromium-mcp-codex.sh` to refresh Codex's
  endpoint and token.
- **Token file is missing:** The server writes runtime files after startup.
  Check that both `--enable-features=BrowserAgentService` and
  `--enable-browser-agent-service` were passed to the AppImage.
- **Codex reports a missing token environment variable:** Start Codex with
  `./tools/chromium-mcp-codex.sh` so it loads the token from the running
  browser's profile.
- **Codex shows `chromium_agent_browser` with 0 tools:** The shared app-server
  daemon may have started before the token was loaded. Rerun the launcher; it
  restarts the daemon before opening Codex.
- **The browser opened without MCP:** Start another instance with the command
  in this guide. MCP must be enabled when Chromium starts.
- **`No usable sandbox` on Ubuntu 24.04:** Use the helper in the Ubuntu
  AppArmor section above. Do not use `--no-sandbox` for normal browsing.

For the protocol details and complete tool catalog, see the
[Tool Contract](TOOL_CONTRACT.md). For setup using the built `chrome` binary,
Claude Code instructions, tests, and other development commands, see the
[Chromium MCP Development and Use Guide](CHROMIUM_MCP_DEVELOPMENT.md).


## Using a local model

1. Download the LLM
```sh
ollama pull fireboi25/qwythos-v2:q4
```

2. Create the Codex-compatible model with 64K context
```sh
cat > /tmp/Modelfile.qwythos-codex <<'EOF'
FROM fireboi25/qwythos-v2:q4
PARAMETER num_ctx 65536
EOF

ollama create qwythos-codex-q4 \
  -f /tmp/Modelfile.qwythos-codex
```

This creates the local model qwythos-codex-q4:latest.

3. Run Codex with the local LLM
This is the command that worked:
```sh
codex -m qwythos-codex-q4 \
  -c 'model_provider="heretic_local"' \
  -c 'model_providers.heretic_local.name="Local Ollama"' \
  -c 'model_providers.heretic_local.base_url="http://127.0.0.1:11434/v1"' \
  -c 'model_providers.heretic_local.wire_api="responses"' \
  --disable apps
```

This configuration:
- Uses your local Ollama server instead of OpenAI's servers.
- Runs Qwythos V2 Q4 with a 64K context window.
- Disables the 336 Codex Apps tools that were causing context overflow.
- Preserves your Chromium MCP server and other configured MCP servers.
- Doesn't require an OpenAI API key for local inference.
