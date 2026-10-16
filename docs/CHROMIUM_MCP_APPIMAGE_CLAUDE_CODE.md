# Use the Chromium MCP AppImage with Claude Code

This guide configures Claude Code to launch Chromium MCP automatically on the
same Linux machine.

## Find the AppImage

In the checkout where the AppImage was built, it is located at:

```text
out/Default/Chromium-MCP-x86_64.AppImage
```

The `out/` directory is ignored by Git. The AppImage is a local build artifact,
so it is not included in a fresh checkout or in the files on GitHub. If yours
is in a different location, pass its path to the setup command. If needed,
make it executable with `chmod +x /path/to/Chromium-MCP-x86_64.AppImage`.

## One-time setup

From this repository checkout, run:

```sh
./tools/chromium-mcp-setup.sh claude
```

The command expects the AppImage at
`out/Default/Chromium-MCP-x86_64.AppImage`. To use another location, pass its
path:

```sh
./tools/chromium-mcp-setup.sh claude /path/to/Chromium-MCP-x86_64.AppImage
```

The setup prepares the AppImage under `~/.local/opt/chromium-mcp`, installs an
AppArmor profile if the system requires one, and adds the
`chromium-agent-browser` MCP entry to Claude Code's user configuration. On
Ubuntu 24.04, `sudo` may prompt once to install the profile; Ubuntu 22.04
normally does not need it. Restart Claude Code after setup, and run Claude
Code from your graphical desktop session so its MCP process can open the
Chromium window.

The MCP bridge launches Chromium with a dedicated profile and a dynamic
loopback port. It reads the short-lived authentication token from the browser
profile and forwards MCP messages over local stdio, so the token does not need
to be copied into Claude Code configuration or exported in a terminal.

In Claude Code, run `/mcp` to confirm that `chromium-agent-browser` lists its
tools. To verify the browser connection, ask Claude Code to call
`browser.version`. The default profile is
`~/.local/share/chromium-mcp-profile`; set `CHROMIUM_MCP_PROFILE_DIR` in the
client's environment to use another profile. Run setup again after replacing
the AppImage to refresh its extracted files.

To prepare and register Chromium MCP with both Codex and Claude Code, use
`./tools/chromium-mcp-setup.sh all`.

## Ubuntu 22.04 and 24.04: AppArmor

Ubuntu 24.04 restricts unprivileged user namespaces through AppArmor by
default. Chromium needs these namespaces for its Linux sandbox and may exit
with `No usable sandbox` before the MCP server starts. Ubuntu 22.04 does not
enable this AppArmor restriction by default, so it normally does not need a
profile. The setup command checks the active restriction: it installs a
profile for the extracted Chromium binary when required, and skips profile
installation when the restriction is off. On Ubuntu 24.04, `sudo` may prompt
once to install the system-wide profile. Do not add `--no-sandbox`.

## Troubleshooting

- **Claude Code does not show Chromium tools:** Confirm the visible browser
  process is still running and restart Claude Code so it reloads the MCP
  configuration.
- **The MCP process fails to start:** Check
  `~/.local/state/chromium-mcp/chromium-mcp.log` for Chromium startup errors.
- **The setup command cannot find the AppImage:** Pass its path explicitly as
  shown above.
- **`abi/4.0` is missing on Ubuntu 22.04:** Rerun setup from an updated
  checkout. It removes the obsolete profile written by older helper versions.
- **`No usable sandbox` on Ubuntu 22.04 or 24.04:** Run the setup command
  again to install or refresh the AppArmor profile. Do not use `--no-sandbox`
  for normal browsing.

For the protocol details and complete tool catalog, see the
[Tool Contract](TOOL_CONTRACT.md). For development setup, tests, and the
non-AppImage connection method, see the
[Chromium MCP Development and Use Guide](CHROMIUM_MCP_DEVELOPMENT.md).
