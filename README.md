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

This fork is being extended into a Chromium-native agent browser. The project
adds a browser-process control plane that can be exposed through an MCP sidecar
while keeping policy decisions inside Chromium. The sidecar adapts external MCP
calls; Chromium remains authoritative for tab state, frame state, input dispatch,
permissions, sensitive-data redaction, downloads/uploads, audit logging, and
trace capture.

Project entry points:

- [Agent Browser Specification](docs/CHROMIUM_MCP_AGENT_BROWSER_SPEC.md)
- [Agent Browser TODO](docs/CHROMIUM_MCP_AGENT_BROWSER_TODO.md)
- [Project Orientation](docs/PROJECT_ORIENTATION.md)
- [Security Model](docs/SECURITY_MODEL.md)
- [Tool Contract](docs/TOOL_CONTRACT.md)
- [Test Plan](docs/TEST_PLAN.md)

Initial architecture summary:

- Chromium fork: owns browser state, policy enforcement, redaction, audit, and
  native browser operations.
- Browser-agent service: planned browser-process service behind an explicit
  feature/runtime flag.
- MCP sidecar: planned local adapter that exposes Playwright-inspired MCP tools
  while forwarding requests to the browser-agent service.
- Agent clients: untrusted callers that receive only schema-validated,
  policy-filtered capabilities.
- User: final authority for sensitive operations such as file crossing,
  privileged permissions, account changes, payments, and other high-risk form
  submissions.

Development is currently in Phase 0. See `docs/PROJECT_ORIENTATION.md` for the
current repository layout, local build/test entry points, branch policy, and CI
capacity notes.
