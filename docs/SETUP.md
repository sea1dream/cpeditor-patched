# Windows setup

1. Extract the portable release to a path without special shell characters,
   for example `D:\CP Editor`.
2. Copy `config/cp_editor_settings.example.ini` beside `cpeditor.exe` and
   rename it to `cp_editor_settings.ini`.
3. Edit these paths for the current machine:
   - `clang_format/program`
   - `cpp/compile_command`
   - `cpp/template_path`
   - `lsp/path_cpp`
   - `lsp/args_cpp`
   - Java and Python commands, if used
4. Install JetBrains Mono to reproduce the configured editor font.
5. Start CP Editor. Competitive Companion listens on port `10045`.

The supplied setup expects a GNU++20-compatible MinGW GCC and clangd. The
author's machine used MSYS2 UCRT64 GCC 15.2 and LLVM/clangd 22.1.8.

Do not copy `cp_editor_session.json` between public machines: it can contain
complete unsaved programs and imported test data.
