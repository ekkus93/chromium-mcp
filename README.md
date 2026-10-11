# ![Logo](chrome/app/theme/chromium/product_logo_64.png) Chromium

## Chromium MCP Agent Browser

This Chromium fork adds an opt-in MCP server inside the browser process. It
exposes browser, context, page, storage, accessibility, network, download, and
input tools through Chromium's native APIs. It does not use CDP, Playwright, or
a separate MCP sidecar. The feature is experimental and currently qualified on
Linux; testing has been limited to Ubuntu 24.04 LTS. Other Linux distributions
and operating systems are unverified. The feature is disabled unless explicitly
enabled at startup.

Start with the [Chromium MCP development guide](docs/CHROMIUM_MCP_DEVELOPMENT.md)
for checkout prerequisites, development setup, build and test commands, MCP
client configuration, and packaging status.
For local AppImage instructions, see the
[AppImage and Codex guide](docs/CHROMIUM_MCP_APPIMAGE_CODEX.md) or
[AppImage and Claude Code guide](docs/CHROMIUM_MCP_APPIMAGE_CLAUDE_CODE.md).

Project references:

- [Tool Contract](docs/TOOL_CONTRACT.md) — available tools and their behavior.
- [AppImage and Codex guide](docs/CHROMIUM_MCP_APPIMAGE_CODEX.md) — launch the
  local AppImage with MCP enabled and connect Codex.
- [AppImage and Claude Code guide](docs/CHROMIUM_MCP_APPIMAGE_CLAUDE_CODE.md) —
  launch the local AppImage with MCP enabled and connect Claude Code.
- [Security Model](docs/SECURITY_MODEL.md) — endpoint, token, profile, and
  browser security boundaries.
- [Test Plan](docs/TEST_PLAN.md) — focused validation and integration suites.
- [Project Orientation](docs/PROJECT_ORIENTATION.md) — repository layout,
  upstream sync, and Chromium build guidance.
- [Agent Browser Specification](docs/CHROMIUM_MCP_AGENT_BROWSER_SPEC.md)
- [Agent Browser TODO](docs/CHROMIUM_MCP_AGENT_BROWSER_TODO.md)
- [Remediation Specification](docs/CHROMIUM_MCP_AGENT_BROWSER_REMEDIATION_SPEC.md)
- [Remediation TODO](docs/CHROMIUM_MCP_AGENT_BROWSER_REMEDIATION_TODO.md)
- [Review Follow-up Specification](docs/CHROMIUM_MCP_AGENT_BROWSER_REVIEW_FOLLOWUP_SPEC.md)
- [Review Follow-up TODO](docs/CHROMIUM_MCP_AGENT_BROWSER_REVIEW_FOLLOWUP_TODO.md)

There is no maintained AppImage build or release recipe in this repository.
See the development guide for the supported Chromium build output and current
packaging scope.

## About Chromium

Chromium is an open-source browser project that aims to build a safer, faster,
and more stable way for all users to experience the web.

The project's web site is https://www.chromium.org.

To check out the source code locally, don't use `git clone`! Instead,
follow [the instructions on how to get the code](docs/get_the_code.md).

Documentation in the source is rooted in [docs/README.md](docs/README.md).

Learn how to [Get Around the Chromium Source Code Directory
Structure](https://www.chromium.org/developers/how-tos/getting-around-the-chrome-source-code).

For historical reasons, there are some small top level directories. Now the
guidance is that new top level directories are for product (e.g. Chrome,
Android WebView, Ash). Even if these products have multiple executables, the
code should be in subdirectories of the product.

If you found a bug, please file it at https://crbug.com/new.
