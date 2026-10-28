# Run the portable Linux build

Extract `OpenXMB-0.3.0-beta.3-linux-x86_64.tar.gz`, then run its `OpenXMB` launcher:

```sh
./OpenXMB-0.3.0-beta.3-linux-x86_64/OpenXMB
./OpenXMB-0.3.0-beta.3-linux-x86_64/OpenXMB --force-dir "/path/to/demo home"
```

The archive includes application libraries, FFmpeg, PDF helpers, fonts and the
WebKit browser resources. It requires an x86-64 Linux system with glibc 2.39 or
newer, a Vulkan-capable host graphics driver, and desktop display services. Its
embedded browser currently uses X11/XWayland and needs host `bwrap`,
`xdg-dbus-proxy`, a working EGL graphics driver, and trusted system certificates.
The archive includes the EGL/GLES client libraries; graphics drivers stay on the
host. FUSE is not required.

Keep the extracted directory together and use the launcher rather than invoking
`bin/XMS.bin` directly. Paths with spaces or Unicode are supported; paths with a
colon are not supported by the Linux library loader. The launcher retains the
caller's current directory, so relative `--force-dir` arguments remain relative
to that directory. A home-style media root can contain `Music`, `Pictures` or
`Photos`, and `Videos` or `Movies`.

Application settings stay in the normal user profile. The archive prefers a
compatible host C++ runtime and otherwise uses its bundled fallback. Host GPU
drivers may require a newer runtime; the archive does not replace graphics
drivers or the system C library. Runner checks cover Ubuntu, Fedora and Arch;
other distributions and physical GPUs may require additional testing.

## Browser user-namespace permissions

Some systems, including Ubuntu with restricted unprivileged user namespaces,
require an AppArmor rule for the extracted browser executable. The archive's
`print-apparmor-profile` prints a narrowly matched profile for its current path:

```sh
./print-apparmor-profile
```

An administrator can review that output and install it using the system's normal
AppArmor profile procedure. The generator does not change the system. Its
profile permits this executable to create user namespaces; WebKit retains its
own bubblewrap and seccomp sandbox. Do not disable the WebKit sandbox. Regenerate
the profile if you move the extracted directory. AppArmor glob and quoting
characters in the path are rejected by the profile generator.

If the browser fails to launch, retain its error log and check the host sandbox
helpers and namespace policy. The installed DEB's `/usr/bin/openxmb-web` rule
does not cover a separately extracted portable archive.
