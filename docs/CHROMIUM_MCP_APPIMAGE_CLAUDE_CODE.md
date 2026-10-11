# Use the Chromium MCP AppImage with Claude Code

This guide starts the MCP server in the Chromium MCP AppImage and connects
Claude Code to it on the same Linux machine.

## Find the AppImage

In the checkout where the AppImage was built, it is located at:

```text
out/Default/Chromium-MCP-x86_64.AppImage
```

The `out/` directory is ignored by Git. The AppImage is a local build artifact,
so it is not included in a fresh checkout or in the files on GitHub. If yours is
in a different location, use that path below. If needed, make it executable
with `chmod +x /path/to/Chromium-MCP-x86_64.AppImage`.

## Start Chromium with MCP enabled

Opening the AppImage normally starts Chromium with MCP disabled. To enable
MCP, launch it with the feature and runtime switches. This uses a dedicated
profile, which Chromium creates on first launch:

```sh
APPIMAGE="$PWD/out/Default/Chromium-MCP-x86_64.AppImage"
MCP_PROFILE="$HOME/.local/share/chromium-mcp-profile"
"$APPIMAGE" \
  --user-data-dir="$MCP_PROFILE" \
  --enable-features=BrowserAgentService \
  --enable-browser-agent-service \
  --browser-agent-mcp-port=17373
```

Keep this Chromium process running while Claude Code uses the MCP server. The
endpoint listens on IPv4 loopback at `127.0.0.1`, port `17373`. Keep Claude
Code on the same machine and do not expose the endpoint through a public
interface, tunnel, or reverse proxy.

If you already opened the AppImage normally, that browser process does not
have MCP enabled. Start another instance with the command above; it uses the
separate `chromium-mcp-profile` profile.

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
the AppImage changes. Leave the terminal running while Claude Code connects.
If no graphical display is available, run the script from a terminal in the
desktop session.

## Load the endpoint token

After Chromium starts, open a second terminal. Export the token without
printing it:

```sh
MCP_RUNTIME="$HOME/.local/share/chromium-mcp-profile/Default/BrowserAgentRuntime"
export CHROMIUM_MCP_TOKEN="$(cat "$MCP_RUNTIME/browser-agent-mcp-token")"
CHROMIUM_MCP_ENDPOINT="$(cat "$MCP_RUNTIME/browser-agent-mcp-endpoint")"
printf 'MCP endpoint: %s\n' "$CHROMIUM_MCP_ENDPOINT"
```

The token is a credential. Do not paste it into source files, check it into
Git, or share it. Keep this terminal open and start Claude Code from it so
Claude Code can read `CHROMIUM_MCP_TOKEN` from its environment.

## Configure Claude Code

Register the HTTP server at user scope so it is available in your Claude Code
projects and its configuration stays in your home directory. The command
stores an environment variable reference rather than the token itself:

```sh
claude mcp add-json chromium-agent-browser \
  '{"type":"http","url":"http://127.0.0.1:17373/mcp","headers":{"Authorization":"Bearer ${CHROMIUM_MCP_TOKEN}"}}' \
  --scope user
```

Claude Code expands `${CHROMIUM_MCP_TOKEN}` from its environment for the HTTP
authorization header. Start Claude Code from the terminal where the token is
exported. Check the connection with:

```sh
claude mcp list
```

Inside Claude Code, run `/mcp` to view the connection status. To verify the
browser tools work, ask Claude Code to call `browser.version` and report the
Chromium build metadata. See the official
[Claude Code MCP guide](https://code.claude.com/docs/en/mcp) for current
configuration and environment variable behavior.

If you chose another MCP port, replace `17373` in the registration command
with the port shown in `browser-agent-mcp-endpoint`. If this server name is
already registered, remove its old entry with
`claude mcp remove chromium-agent-browser` and run the registration command
again.

## Troubleshooting

- **Claude Code cannot connect:** Confirm Chromium is still running and the
  endpoint uses `127.0.0.1` with the same port as the endpoint file.
- **Token file is missing:** The server writes runtime files after startup.
  Check that both `--enable-features=BrowserAgentService` and
  `--enable-browser-agent-service` were passed to the AppImage.
- **Claude Code reports a missing environment variable:** Start it from the
  terminal where `CHROMIUM_MCP_TOKEN` was exported.
- **The browser opened without MCP:** MCP must be enabled when Chromium starts.
  Start it again using the launch command above.
- **`No usable sandbox` on Ubuntu 24.04:** Use the helper in the Ubuntu
  AppArmor section. Do not use `--no-sandbox` for normal browsing.

For the protocol details and complete tool catalog, see the
[Tool Contract](TOOL_CONTRACT.md). For development setup, tests, and the
non-AppImage connection method, see the
[Chromium MCP Development and Use Guide](CHROMIUM_MCP_DEVELOPMENT.md).
