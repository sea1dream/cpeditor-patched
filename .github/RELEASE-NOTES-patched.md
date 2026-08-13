## CP Editor 7.0.2 patched.2

Unofficial personal Windows x64 portable build based on CP Editor 7.0.2.

Highlights:

- fixes the large-template/session-restore syntax-highlighter crash;
- adds clangd completion popups;
- realtime one-character and member-access suggestions;
- immediate local filtering plus 80 ms clangd refresh;
- clangd semantic colors for C++ functions, types, parameters, variables,
  constants, keywords, and operators;
- Dracula-style operators and rainbow bracket pairs, with comments, strings,
  numbers, and `#include` paths preserved;
- guarded semantic refreshes without stale overlays or background request
  loops;
- stale-response, UTF-16, URI, transport, and editor-lifetime fixes;
- includes a sanitized Dracula/JetBrains Mono/clang-format configuration and
  the personal GNU++20 template.

Assets include the portable Windows build, complete corresponding source with
all submodules, and SHA-256 checksums.

See `README-PATCHED.md` and `docs/SETUP.md` for details. This is not an official
CP Editor release.
