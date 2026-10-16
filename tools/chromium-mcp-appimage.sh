#!/usr/bin/env bash
set -Eeuo pipefail

PREPARE_ONLY=0
if [[ "${1:-}" == "--prepare" ]]; then
  PREPARE_ONLY=1
  shift
fi
if [[ $# -gt 1 ]]; then
  printf 'Usage: %s [--prepare] [AppImage]\n' "$0" >&2
  exit 2
fi

APPIMAGE="${1:-$PWD/out/Default/Chromium-MCP-x86_64.AppImage}"
if [[ ! -f "$APPIMAGE" ]]; then
  printf 'AppImage not found: %s\n' "$APPIMAGE" >&2
  printf 'Run this script from the checkout, or pass the AppImage path as its argument.\n' >&2
  exit 1
fi
APPIMAGE="$(realpath "$APPIMAGE")"
if [[ ! -x "$APPIMAGE" ]]; then
  printf 'AppImage is not executable: %s\n' "$APPIMAGE" >&2
  exit 1
fi

EXTRACT_DIR="$HOME/.local/opt/chromium-mcp"
APPDIR="$EXTRACT_DIR/squashfs-root"
PROFILE_FILE="/etc/apparmor.d/chromium-mcp"
APPARMOR_USERNS_RESTRICTION="/proc/sys/kernel/apparmor_restrict_unprivileged_userns"
HASH_FILE="$EXTRACT_DIR/.appimage-sha256"
mkdir -p "$EXTRACT_DIR"
APPIMAGE_HASH="$(sha256sum "$APPIMAGE" | awk '{print $1}')"

if [[ ! -x "$APPDIR/chrome" || ! -x "$APPDIR/AppRun" || ! -f "$HASH_FILE" || "$(cat "$HASH_FILE")" != "$APPIMAGE_HASH" ]]; then
  STAGING_DIR="$(mktemp -d "$EXTRACT_DIR/.extract.XXXXXX")"
  trap 'rm -rf "$STAGING_DIR"' EXIT
  printf 'Extracting AppImage to %s ...\n' "$APPDIR"
  (cd "$STAGING_DIR" && "$APPIMAGE" --appimage-extract >/dev/null)
  if [[ ! -x "$STAGING_DIR/squashfs-root/chrome" || ! -x "$STAGING_DIR/squashfs-root/AppRun" ]]; then
    printf 'The AppImage extraction is missing chrome or AppRun.\n' >&2
    exit 1
  fi
  if [[ -e "$APPDIR" ]]; then
    BACKUP_DIR="$EXTRACT_DIR/squashfs-root.backup.$(date +%Y%m%d%H%M%S)"
    printf 'Keeping previous extraction at %s\n' "$BACKUP_DIR"
    mv "$APPDIR" "$BACKUP_DIR"
  fi
  mv "$STAGING_DIR/squashfs-root" "$APPDIR"
  printf '%s\n' "$APPIMAGE_HASH" > "$HASH_FILE"
  rm -rf "$STAGING_DIR"
  trap - EXIT
else
  printf 'Using the existing extraction at %s\n' "$APPDIR"
fi

CHROME_BIN="$APPDIR/chrome"
if [[ "$CHROME_BIN" == *[[:space:]]* ]]; then
  printf 'AppArmor profile path contains whitespace, which this script does not support: %s\n' "$CHROME_BIN" >&2
  exit 1
fi

if [[ -r "$APPARMOR_USERNS_RESTRICTION" ]] &&
    [[ "$(cat "$APPARMOR_USERNS_RESTRICTION")" == 1 ]]; then
  if [[ ! -r /etc/apparmor.d/abi/4.0 ]]; then
    printf 'AppArmor is restricting unprivileged user namespaces, but /etc/apparmor.d/abi/4.0 is missing.\n' >&2
    printf 'Repair or update the AppArmor package for this Ubuntu release, then rerun this script.\n' >&2
    exit 1
  fi

  PROFILE_TMP="$(mktemp)"
  trap 'rm -f "$PROFILE_TMP"' EXIT
  cat > "$PROFILE_TMP" <<EOF
abi <abi/4.0>,
include <tunables/global>

profile chromium-mcp $CHROME_BIN flags=(unconfined) {
  userns,
}
EOF

  printf 'Installing AppArmor profile (sudo may prompt for your password) ...\n'
  sudo install -o root -g root -m 0644 "$PROFILE_TMP" "$PROFILE_FILE"
  sudo apparmor_parser -r "$PROFILE_FILE"
  rm -f "$PROFILE_TMP"
  trap - EXIT
else
  if [[ -e "$PROFILE_FILE" ]]; then
    printf 'Removing the stale Chromium MCP AppArmor profile; this system does not need it.\n'
    sudo apparmor_parser -R "$PROFILE_FILE" >/dev/null 2>&1 || true
    sudo rm -f "$PROFILE_FILE"
  fi
  printf 'AppArmor user namespace restriction is not enabled; no AppArmor profile is needed.\n'
fi

if [[ "$PREPARE_ONLY" == 1 ]]; then
  printf 'Chromium MCP AppImage is ready for Codex or Claude Code.\n'
  exit 0
fi

if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
  printf 'No graphical display is available in this shell. Open a terminal in your Ubuntu desktop session and rerun this script.\n' >&2
  exit 1
fi

MCP_PROFILE="$HOME/.local/share/chromium-mcp-profile"
printf 'Starting Chromium MCP. Keep this process running while Codex connects.\n'
"$APPDIR/AppRun" \
  --user-data-dir="$MCP_PROFILE" \
  --enable-features=BrowserAgentService \
  --enable-browser-agent-service \
  --browser-agent-mcp-port=17373
