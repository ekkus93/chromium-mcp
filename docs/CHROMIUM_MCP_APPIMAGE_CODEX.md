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
in a different location, pass its path to the setup command below. If needed,
make it executable with `chmod +x /path/to/Chromium-MCP-x86_64.AppImage`.

## One-time setup

The setup command prepares the AppImage and registers Chromium MCP with Codex.
Codex then launches Chromium automatically when it starts the MCP server; you
do not need to start Chromium in a separate terminal.

From this repository checkout, run:

```sh
./tools/chromium-mcp-setup.sh codex
```

The command expects the AppImage at
`out/Default/Chromium-MCP-x86_64.AppImage`. To use another location, pass its
path:

```sh
./tools/chromium-mcp-setup.sh codex /path/to/Chromium-MCP-x86_64.AppImage
```

The setup prepares the AppImage under `~/.local/opt/chromium-mcp`, installs an
AppArmor profile if the system requires one, and adds the
`chromium_agent_browser` MCP entry to Codex's user configuration. On Ubuntu
24.04, `sudo` may prompt once to install the profile; Ubuntu 22.04 normally
does not need it. Restart Codex after setup, and run Codex from your graphical
desktop session so its MCP process can open the Chromium window.

The MCP bridge launches Chromium with a dedicated profile and a dynamic
loopback port. It reads the short-lived authentication token from the browser
profile and forwards MCP messages over local stdio, so the token does not need
to be copied into Codex configuration or exported in a terminal.

In the Codex TUI, use `/mcp` to confirm that `chromium_agent_browser` lists its
tools. To verify the browser connection, ask Codex to call `browser.version`.
The default profile is `~/.local/share/chromium-mcp-profile`; set
`CHROMIUM_MCP_PROFILE_DIR` in the client's environment to use another profile.
Run setup again after replacing the AppImage to refresh its extracted files.

### Ubuntu 22.04 and 24.04: AppArmor

Ubuntu 24.04 restricts unprivileged user namespaces through AppArmor by
default. Chromium needs these namespaces for its Linux sandbox and may exit
with `No usable sandbox` before the MCP server starts. Ubuntu 22.04 does not
enable this AppArmor restriction by default, so it normally does not need a
profile. The setup command checks the active restriction: it installs a
profile for the extracted Chromium binary when required, and skips profile
installation when the restriction is off. On Ubuntu 24.04, `sudo` may prompt
once to install the system-wide profile. Do not add `--no-sandbox`.

To configure both Codex and Claude Code in one command, run
`./tools/chromium-mcp-setup.sh all`.

## Run Codex with `--yolo`

After the one-time setup above, start Codex with:

```sh
codex --yolo
```

Chromium MCP starts automatically when Codex starts its configured MCP server,
so no separate Chromium MCP command is needed. If you normally start Codex with
other options, add `--yolo` to that command. For example, append `--yolo` to
the local model command below.

`--yolo` disables Codex approval prompts and sandboxing for the session. Use it
only in an environment you have secured for unrestricted command execution.

## Troubleshooting

- **Codex does not show Chromium tools:** Confirm the visible browser process
  is still running and restart Codex so it reloads the MCP configuration.
- **The MCP process fails to start:** Check
  `~/.local/state/chromium-mcp/chromium-mcp.log` for Chromium startup errors.
- **The setup command cannot find the AppImage:** Pass its path explicitly as
  shown above.
- **`abi/4.0` is missing on Ubuntu 22.04:** Rerun setup from an updated
  checkout. It removes the obsolete profile written by older helper versions.
- **`No usable sandbox` on Ubuntu 22.04 or 24.04:** Rerun the setup command
  to install or refresh the AppArmor profile. Do not use `--no-sandbox` for
  normal browsing.

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
