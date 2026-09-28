# Repository Guidelines

## Project Structure & Module Organization

This repository is the Chromium source tree. Product code is grouped by area: `chrome/` contains desktop Chrome, `content/` the web platform and browser core, `components/` shared features, `base/` common infrastructure, and `third_party/` vendored dependencies. Tests usually live beside the code or in area-specific `test/` directories; Web Tests are under `third_party/blink/web_tests/`. Build targets and source membership are defined in `BUILD.gn` files. Start with `docs/README.md` and the relevant directory README before changing an unfamiliar area.

## Build, Test, and Development Commands

Chromium requires `depot_tools` and a configured output directory; see `docs/get_the_code.md` and `docs/building/` for setup and platform details. Typical Linux commands are:

- `gn gen out/Default` — generate Ninja build files (configure arguments with `gn args out/Default`).
- `autoninja -C out/Default chrome` — build Chrome; replace `chrome` with the affected target when possible.
- `autoninja -C out/Default base_unittests` — build a component’s unit-test binary.
- `out/Default/base_unittests` — run that binary; use `--gtest_filter=Suite.Test` to select a test.
- `third_party/blink/tools/run_web_tests.py -t Default fast/forms` — run a Web Test subset.

## Coding Style & Naming Conventions

Follow nearby code and the language-specific rules in `docs/`. C++ formatting follows `.clang-format`; use Chromium naming patterns (types and methods in `UpperCamelCase`, variables in `lower_snake_case`). Python uses the repository’s Python tooling; JavaScript and TypeScript conventions are directory-specific. Keep changes focused and update `BUILD.gn` and tests when adding sources or behavior.

## Testing Guidelines

Add or update tests with the code they cover. Chromium uses GoogleTest for most C++ unit and browser tests, plus Web Tests and platform-specific suites. Build and run the narrowest relevant target first. Follow `docs/testing/testing_in_chromium.md` and the affected area’s instructions; there is no single repository-wide test command or coverage threshold.

## Commit & Pull Request Guidelines

Chromium uses Gerrit for review. Upload changes with `git cl upload` and follow `docs/contributing.md` and the commit checklist. Use a concise subject, a blank line, a wrapped explanation of the reason and behavior, and relevant footers such as `Bug: 123456` and `Test: ...`. Include test results and reviewer context; add screenshots for user-visible UI changes when useful.

## Security & Configuration Tips

Do not commit secrets, credentials, local build output, or generated files. Keep GN arguments and build artifacts in `out/`; report security-sensitive vulnerabilities through Chromium’s security reporting process rather than a public issue.
