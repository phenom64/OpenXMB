# Linux portable archive integration

This packet is a candidate for the Beta 3 release, not evidence of a tested
release. Build on Ubuntu 24.04 x86-64, from the trusted CMake install stage:

```sh
python3 scripts/packaging/linux_portable.py \
  --stage "$STAGE" --output "$OUTPUT" --version 0.3.0-beta.3
```

It produces `OpenXMB-0.3.0-beta.3-linux-x86_64.tar.gz` and its `.sha256`.
The builder requires Python 3, patchelf, ldd, readelf, dpkg-query,
gio-querymodules, FFmpeg, Poppler tools, WebKitGTK 4.1 with its process
executables, and glib-networking. The release build driver provisions pinned
Noto fonts and OFL notices into the install stage; the portable builder requires
those files in `share/shell/fonts`. It uses ldd, so the stage and all
build-host libraries must be trusted. Build on glibc 2.39; the archive does not
contain glibc, the ELF loader, or GPU driver implementations. Drivers, display
services, `bwrap`, `xdg-dbus-proxy`, and system certificates are host needs.
The bundled Vulkan loader discovers the host's ICDs. Paths containing `:` are
not supported by the ELF loader's search-path environment variable.

The launcher keeps the caller's working directory and arguments, including
relative `--force-dir` paths. It selects a compatible host `libstdc++` and
`libgcc_s` before using the bundled fallbacks. If a fallback is necessary, a
host GPU driver can still require newer symbols than that fallback provides;
test that case on the target system. Packaged GIO modules include the TLS
backend. The staged Noto fonts, OFL licenses, and copyright notices are preserved
in `share/shell/fonts`.

Runner invocation (the archive is extracted to a temporary path containing
spaces and non-ASCII characters):

```sh
python3 scripts/packaging/portable_smoke.py --archive "$ARCHIVE" \
  --output "$SMOKE_OUTPUT"
```

This smoke needs Pillow and tests the launcher, assets, catalogs, FFmpeg,
Poppler, integration helpers, a relative `--force-dir` and headless Vulkan
output. It intentionally
reports that the browser was not tested. Run it on Ubuntu 24.04, Fedora and
Arch with a working host Vulkan ICD, after hiding build-tree paths and host
FFmpeg/Poppler, and separately verify that a host with a newer C++ runtime uses
its own runtime. Containers do not prove physical-GPU behavior.

## Browser release gate

Ubuntu release WebKitGTK embeds its process directory in the shared library;
the developer-only `WEBKIT_EXEC_PATH` override is insufficient. The builder
copies the matching WebKit processes and rewrites the exact embedded directory
to a relative bundle path; it fails if it cannot find that path unambiguously.
Only the browser child changes its working directory. The launcher places the
bundle directories in `LD_LIBRARY_PATH`, which WebKit's bubblewrap code uses
as sandbox bind paths. The sandbox is never disabled. This is an unverified
relocation strategy until a real browser integration test succeeds: verify
matching WebKit library and processes, an actual sandboxed HTTPS page load,
TLS, user data, process lifetime, and resource loading on each distribution,
including an extraction path with spaces and Unicode. Also verify the host's
`bwrap` and `xdg-dbus-proxy` work with the inherited library environment. If
these fail, do not release this packet as browser-capable; a parent-owned
source change or another verified runtime layout may be required.

On Ubuntu with AppArmor namespace restrictions, the installed DEB profile for
`/usr/bin/openxmb-web` does not apply to this archive. The archive includes
`print-apparmor-profile`, which prints a profile for the exact extracted
`bin/openxmb-web.bin` path. It does not install anything. An administrator can
review its output and, if appropriate, install it with `apparmor_parser` and
their normal local profile procedure. The profile matches this executable
only, permits user namespaces, and otherwise uses AppArmor's unconfined mode;
it does not disable WebKit's sandbox or change system-wide namespace policy.
Regenerate it after moving the archive. Its generator rejects AppArmor pattern
and quoting characters in the path. CI's existing profile matching
`openxmb-web` must be adjusted for the `.bin` executable.

Sources: [WebKit process path selection](https://github.com/WebKit/WebKit/blob/main/Source/WebKit/Shared/glib/ProcessExecutablePathGLib.cpp),
[WebKit bubblewrap launcher](https://github.com/WebKit/WebKit/blob/main/Source/WebKit/UIProcess/Launcher/glib/BubblewrapLauncher.cpp),
and [Ubuntu AppArmor documentation](https://documentation.ubuntu.com/security/security-features/privilege-restriction/apparmor/).
