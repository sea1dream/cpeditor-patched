# CP Editor 7.0.2 — personal patched build

This is an unofficial Windows x64 build based on
[CP Editor 7.0.2](https://github.com/cpeditor/cpeditor/tree/7.0.2). It keeps the
portable layout and adds fixes developed for competitive-programming use.

## Changes

- Fixes the startup crash caused by queuing a stale `QTextBlock` while a large
  default template is replaced during session restoration.
- Adds clangd-backed C++ completion with a themed popup.
- Shows automatic suggestions after the first character and after `.`, `->`,
  and `::`.
- Filters cached results immediately while typing, refreshes clangd after an
  80 ms debounce, and supports abbreviated matches such as `pb` -> `push_back`.
- Handles split/combined LSP frames, unique request IDs, UTF-16 positions,
  stale responses, and document/tab lifetime changes.

## Repository layout

- `config/cp_editor_settings.example.ini`: sanitized copy of the personal
  Dracula / JetBrains Mono / clang-format / Competitive Companion setup.
- `config/compile_commands.example.json`: clangd database template.
- `config/templates/luogu-template.cpp`: personal GNU++20 template.
- `docs/SETUP.md`: installation and path configuration.
- `third_party/lsp-cpp`: points to the matching patched LSP client branch.

The live `cp_editor_session.json` is intentionally excluded because it may
contain unsaved source code, tests, problem URLs, and local paths.

## Build

Clone recursively, then follow the upstream build instructions:

```text
git clone --recursive https://github.com/sea1dream/cpeditor-patched.git
```

The published Windows package is a Release, portable build using Qt 5.15.2
MSVC2019 x64. The exact binary checksum is included with each release.

## License

CP Editor is licensed under GPL-3.0-or-later. See `LICENSE`. Third-party
components retain their respective licenses in their source directories.

This repository and its releases are unofficial and are not endorsed by the
upstream CP Editor project.
