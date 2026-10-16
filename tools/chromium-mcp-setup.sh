#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
APPIMAGE="${2:-$REPO_DIR/out/Default/Chromium-MCP-x86_64.AppImage}"
TARGET="${1:-all}"
APPDIR="$HOME/.local/opt/chromium-mcp/squashfs-root"
BRIDGE="$SCRIPT_DIR/chromium-mcp-stdio.py"
INSTALL_DIR="$HOME/.local/lib/chromium-mcp"
INSTALLED_BRIDGE="$INSTALL_DIR/chromium-mcp-stdio.py"

usage() {
  cat <<'EOF'
Usage:
  tools/chromium-mcp-setup.sh [codex|claude|all] [AppImage]

Prepare the AppImage and register its automatically launched stdio bridge
with Codex, Claude Code, or both. The clients start Chromium with their MCP
server when the client starts.
EOF
}

if [[ "$TARGET" == --help || "$TARGET" == -h ]]; then
  usage
  exit 0
fi

if [[ "$TARGET" != codex && "$TARGET" != claude && "$TARGET" != all ]]; then
  usage >&2
  exit 2
fi
if [[ $# -gt 2 ]]; then
  usage >&2
  exit 2
fi
if [[ ! -x "$APPIMAGE" ]]; then
  printf 'AppImage is missing or not executable: %s\n' "$APPIMAGE" >&2
  printf 'Pass its path explicitly or make it executable with chmod +x.\n' >&2
  exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
  printf 'python3 was not found on PATH.\n' >&2
  exit 1
fi

if [[ "$TARGET" == codex || "$TARGET" == all ]]; then
  if ! command -v codex >/dev/null 2>&1; then
    if [[ "$TARGET" == codex ]]; then
      printf 'Codex CLI was not found on PATH.\n' >&2
      exit 1
    fi
    TARGET="claude"
  fi
fi
if [[ "$TARGET" == claude || "$TARGET" == all ]]; then
  if ! command -v claude >/dev/null 2>&1; then
    if [[ "$TARGET" == claude ]]; then
      printf 'Claude Code CLI was not found on PATH.\n' >&2
      exit 1
    fi
    if ! command -v codex >/dev/null 2>&1; then
      printf 'Neither Codex nor Claude Code CLI was found on PATH.\n' >&2
      exit 1
    fi
    TARGET="codex"
  fi
fi

bash "$SCRIPT_DIR/chromium-mcp-appimage.sh" --prepare "$APPIMAGE"
mkdir -p "$INSTALL_DIR"
install -m 0755 "$BRIDGE" "$INSTALLED_BRIDGE"

configure_codex() {
  if codex mcp get chromium_agent_browser >/dev/null 2>&1; then
    codex mcp remove chromium_agent_browser
  fi
  codex mcp add chromium_agent_browser \
    --config mcp_servers.chromium_agent_browser.startup_timeout_sec=90 -- \
    python3 "$INSTALLED_BRIDGE" --appdir "$APPDIR"
  printf 'Registered chromium_agent_browser with Codex.\n'
}

configure_claude() {
  claude mcp remove chromium-agent-browser >/dev/null 2>&1 || true
  claude mcp add --scope user --transport stdio chromium-agent-browser -- \
    python3 "$INSTALLED_BRIDGE" --appdir "$APPDIR"
  printf 'Registered chromium-agent-browser with Claude Code.\n'
}

case "$TARGET" in
  codex) configure_codex ;;
  claude) configure_claude ;;
  all)
    configure_codex
    configure_claude
    ;;
esac

printf 'Setup complete. Restart the configured client; it will launch Chromium MCP automatically.\n'
