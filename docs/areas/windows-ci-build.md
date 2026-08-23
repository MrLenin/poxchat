# Windows CI build

How `.github/workflows/windows-build.yml` and `win32/ci/` fit together, and
how to iterate on them without spending a runner cycle per question. Useful
when the Windows build breaks, when a dependency version moves under it, or
when re-enabling one of the targets it currently leaves out.

## Shape

| Piece | Does |
|-------|------|
| `win32/ci/build-deps.ps1` | Builds the whole dependency stack into `C:\gtk-build` — gvsbuild for GTK4/OpenSSL/libxml2/sqlite/libcurl/enchant/gettext, then static jansson and libwebsockets, LuaJIT into the gvsbuild prefix, the CPython embeddable runtime, prebuilt WinSparkle, and the CA bundle |
| `win32/ci/check-imports.ps1` | Walks PE imports of the staged tree and names every DLL it is missing |
| `win32/ci/make-installer.ps1` | Generates `poxchat.iss` from its template and runs Inno Setup 5 |

A cold dependency stack is about three quarters of an hour; warm, a run is
roughly 15 minutes. That ratio is why `C:\gtk-build` is cached whole and keyed
on `build-deps.ps1`'s hash, with a `restore-keys` prefix so that editing the
script resumes from the previous stack instead of rebuilding it. Bump
`DEPS_CACHE_EPOCH` when you want a deliberate clean slate.

Cache scope is worth knowing: a pull request run can read caches from its base
branch, not from the topic branch the work was done on, so the first PR run
after a branch has been iterating in isolation pays for a cold stack.

## The rule that matters: never learn one fact per run

At 15 minutes a cycle, anything that discovers problems one at a time is the
slow path. Two steps exist purely to batch that discovery, and both were added
after paying the toll:

- **Inventory the dependency prefix** lists every `.lib` and `.exe`/`.dll` in
  the gvsbuild prefix. `poxchat.props`'s `DepLibs` names import libraries, and
  gvsbuild does not always spell them the same way — `libcurl_imp.lib` vs
  `libcurl.lib`, `libenchant.dll` vs `libenchant-2.dll`. Diff the list against
  what the build asks for instead of guessing one name per run.
- **Check the staged tree's imports** reads the PE import and delay-import
  tables directly. Windows reports a missing dependency as
  `STATUS_DLL_NOT_FOUND` (0xC0000135) at process start with no name attached,
  so the smoke test can only say *broken*. When a missing DLL turns up in the
  dependency prefix the checker follows it and walks its imports too, so a
  chain like `libcurl.dll -> psl-5.dll -> icuuc78.dll` is reported in one pass.

The same instinct applies off the runner. Every `Source:` line in
`win32/installer/poxchat.iss.tt` can be checked against a downloaded portable
zip locally; Inno Setup aborts on the first one that matches nothing, so
auditing all 72 at once is the difference between one cycle and fourteen.

## Validating the PowerShell without Windows

Both CI scripts can be exercised on Linux, which is worth doing — a bare
syntax error in a late step costs a full run:

```sh
# parse check
docker run --rm -v "$PWD/win32/ci:/ci:ro" mcr.microsoft.com/powershell \
  pwsh -NoProfile -Command '$e=$null;$t=$null
    [System.Management.Automation.Language.Parser]::ParseFile("/ci/check-imports.ps1",[ref]$t,[ref]$e)|Out-Null
    if($e){$e|%{"{0}: {1}" -f $_.Extent.StartLineNumber,$_.Message}}else{"ok"}'

# behaviour check -- point it at any directory of real PE files
docker run --rm -v "$PWD/win32/ci:/ci:ro" -v /some/dlls:/tree:ro \
  mcr.microsoft.com/powershell pwsh -NoProfile -File /ci/check-imports.ps1 -Root /tree
```

`check-imports.ps1` runs fine on Linux: `$env:SystemRoot` is empty there, so
the System32 scan is skipped and every system DLL reports as missing, which is
harmless when what you are checking is the set of imports it discovers. A wine
prefix is a convenient corpus of real 32- and 64-bit PEs to test against.

Watch for PowerShell-vs-C reflexes. `for ($i = 0; $i -lt $n; $i++, $p += 40)`
is a parse error: the repeat clause takes one expression, not a comma list.

## The scripting plugins

- **lua** builds against a LuaJIT that `build-deps.ps1` compiles itself and
  installs into the gvsbuild prefix (gvsbuild's own recipe runs `.\msvcbuild`
  through `CreateProcess`, which cannot launch a `.bat`, and fails identically
  every time). lgi and `--enable-gi` are deliberately not built — the plugin
  needs neither, they only ever provided GObject bindings to Lua scripts.
- **python3** links the import library out of setup-python's install and ships
  CPython's *embeddable package*, staged whole into the tree, so users need no
  system Python. The plugin is a cffi embedding: `generate_plugin.py` needs
  `cffi` in the build interpreter, and at run time `_cffi_backend.pyd`
  (staged out of that interpreter's site-packages by `copy.vcxproj`) must sit
  on sys.path — the `.` entry in `python313._pth` provides that.

  The Python minor version is pinned in four places that must move together:
  `python-version` in the workflow, `Python3Lib` in `win32/poxchat.props`,
  `$PythonEmbedVersion`/`$PythonEmbedSha256` in `build-deps.ps1`, and the
  `python313.*` names in `poxchat.iss.tt`.

## Deliberately dropped

- **perl** — dropped as a supported plugin on Windows; it would need a
  matching Strawberry Perl on the runner and on every user's machine.
- **htm / thememan** (the C# theme manager) — needs a .NET toolchain, and the
  installer no longer offers it.

The installer template names only payloads the build produces, so
`-f installer=true` is expected to work; the portable zip still uploads before
the installer step, so an installer failure costs nothing else.

## Traps worth remembering

- `meson_options.txt` defaults `text-frontend` to **false**, so nothing on
  Linux compiles `src/fe-text/fe-text.c`. The Windows solution builds it
  unconditionally, which makes a Windows CI run the only thing that notices
  when its stubs drift out of step with `src/common/fe.h`.
- Every project links the same `$(DepLibs)`, so the search path for it belongs
  in the props' own `Link` defaults, not per project. jansson and libwebsockets
  live outside the gvsbuild prefix that `$(DepsRoot)\lib` points at.
- `win32/poxchat.props` and `win32/copy/copy.vcxproj` are CRLF. Edit them as
  bytes, or a tool that rewrites line endings turns a two-line change into a
  whole-file diff.
- ICU is 35 MB of the staged tree, reached only as
  `libcurl -> libpsl -> ICU`, and libpsl is there for public-suffix checks on
  cookies. Dropping it means building libcurl without libpsl in gvsbuild.
