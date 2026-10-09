# ![Logo](chrome/app/theme/chromium/product_logo_64.png) Chromium

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

## Chromium MCP Agent Browser

This fork adds an MCP server inside Chromium. MCP requests are handled by a
browser-process service that calls Chromium's native browser APIs directly. It
does not use CDP, Playwright, or a separate MCP sidecar to control the browser.

The MCP surface is intended to expose the browser's full native capabilities,
without an agent-specific policy allowlist or approval layer. Chromium's normal
platform boundaries still apply, including operating-system permissions,
profile ownership, and the browser sandbox.

Project entry points:

- [Agent Browser Specification](docs/CHROMIUM_MCP_AGENT_BROWSER_SPEC.md)
- [Agent Browser TODO](docs/CHROMIUM_MCP_AGENT_BROWSER_TODO.md)
- [Agent Browser Remediation Specification](docs/CHROMIUM_MCP_AGENT_BROWSER_REMEDIATION_SPEC.md)
- [Agent Browser Remediation TODO](docs/CHROMIUM_MCP_AGENT_BROWSER_REMEDIATION_TODO.md)
- [Project Orientation](docs/PROJECT_ORIENTATION.md)
- [Security Model](docs/SECURITY_MODEL.md)
- [Tool Contract](docs/TOOL_CONTRACT.md)
- [Test Plan](docs/TEST_PLAN.md)

The in-process MCP service is disabled by default and requires an explicit
feature/runtime opt-in. See [Project Orientation](docs/PROJECT_ORIENTATION.md)
for build and development commands.
