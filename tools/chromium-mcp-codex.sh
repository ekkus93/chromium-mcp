#!/usr/bin/env bash
set -Eeuo pipefail

SERVER_NAME="chromium_agent_browser"
MCP_RUNTIME="${CHROMIUM_MCP_RUNTIME_DIR:-$HOME/.local/share/chromium-mcp-profile/Default/BrowserAgentRuntime}"

usage() {
  cat <<'EOF'
Usage:
  tools/chromium-mcp-codex.sh [setup]

With no argument, load the running Chromium MCP token, make sure Codex is
configured for its endpoint, refresh the app-server daemon if its token changed,
and start Codex. Refreshing a running daemon disconnects its existing sessions.

Use "setup" to configure Codex without starting it. Start the MCP-enabled
Chromium AppImage first so its endpoint and token files exist.
EOF
}

if [[ $# -gt 1 || ( $# -eq 1 && "$1" != "setup" ) ]]; then
  usage >&2
  exit 2
fi

if ! command -v codex >/dev/null 2>&1; then
  printf 'Codex CLI was not found on PATH.\n' >&2
  exit 1
fi

ENDPOINT_FILE="$MCP_RUNTIME/browser-agent-mcp-endpoint"
TOKEN_FILE="$MCP_RUNTIME/browser-agent-mcp-token"
if [[ ! -r "$ENDPOINT_FILE" || ! -r "$TOKEN_FILE" ]]; then
  printf 'Chromium MCP runtime files were not found in %s.\n' "$MCP_RUNTIME" >&2
  printf 'Start the MCP-enabled browser with tools/chromium-mcp-appimage.sh first.\n' >&2
  printf 'Override the runtime directory with CHROMIUM_MCP_RUNTIME_DIR if needed.\n' >&2
  exit 1
fi

CHROMIUM_MCP_ENDPOINT="$(tr -d '\r\n' < "$ENDPOINT_FILE")"
CHROMIUM_MCP_TOKEN="$(tr -d '\r\n' < "$TOKEN_FILE")"
if [[ ! "$CHROMIUM_MCP_ENDPOINT" =~ ^http://127\.0\.0\.1:[0-9]+/mcp$ ]]; then
  printf 'Unexpected Chromium MCP endpoint: %s\n' "$CHROMIUM_MCP_ENDPOINT" >&2
  printf 'Expected an HTTP loopback endpoint ending in /mcp.\n' >&2
  exit 1
fi
if [[ -z "$CHROMIUM_MCP_TOKEN" ]]; then
  printf 'The Chromium MCP token file is empty. Restart the MCP-enabled browser.\n' >&2
  exit 1
fi

CONFIG_CHANGED=0
configure_codex() {
  local current
  if current="$(codex mcp get "$SERVER_NAME" 2>/dev/null)" &&
      [[ "$current" == *"url: $CHROMIUM_MCP_ENDPOINT"* ]] &&
      [[ "$current" == *"bearer_token_env_var: CHROMIUM_MCP_TOKEN"* ]]; then
    printf 'Codex MCP entry "%s" is already configured.\n' "$SERVER_NAME"
    return
  fi

  if [[ -n "${current:-}" ]]; then
    codex mcp remove "$SERVER_NAME"
  fi
  codex mcp add "$SERVER_NAME" \
    --url "$CHROMIUM_MCP_ENDPOINT" \
    --bearer-token-env-var CHROMIUM_MCP_TOKEN
  CONFIG_CHANGED=1
}

configure_codex

if [[ "${1:-}" == "setup" ]]; then
  printf 'Codex is configured. Start it with this script after the MCP browser is running.\n'
  exit 0
fi

export CHROMIUM_MCP_TOKEN
export CHROMIUM_MCP_ENDPOINT

DAEMON_STATUS="$(codex app-server daemon version 2>/dev/null || true)"
TOKEN_HASH="$(printf '%s' "$CHROMIUM_MCP_TOKEN" | sha256sum | awk '{print $1}')"
STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/chromium-mcp"
STATE_FILE="$STATE_DIR/codex-token-sha256"
if [[ "$DAEMON_STATUS" == *'"status":"running"'* ]]; then
  PREVIOUS_TOKEN_HASH="$(cat "$STATE_FILE" 2>/dev/null || true)"
  if [[ "$CONFIG_CHANGED" == 1 || "$PREVIOUS_TOKEN_HASH" != "$TOKEN_HASH" ]]; then
    printf 'Refreshing the Codex app-server daemon for the current MCP configuration and token.\n'
    printf 'Any existing Codex sessions on this daemon will disconnect.\n'
    codex app-server daemon stop
  else
    printf 'The Codex app-server already has the current Chromium MCP token.\n'
  fi
fi
codex app-server daemon start
mkdir -p "$STATE_DIR"
chmod 0700 "$STATE_DIR"
umask 077
printf '%s\n' "$TOKEN_HASH" > "$STATE_FILE"

printf 'Starting Codex with Chromium MCP configured. Use /mcp to verify its tools.\n'
exec codex
