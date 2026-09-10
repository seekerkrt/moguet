# Third-Party Notices

This document records components used by the current GPL-licensed Moguet development series. It distinguishes code linked or compiled into Moguet from programs invoked across a process boundary. The project licensing policy is in [`docs/LICENSING.md`](docs/LICENSING.md) in the source tree and `${PREFIX}/share/doc/moguet/docs/LICENSING.md` after installation (`/usr/share/doc/moguet/docs/LICENSING.md` in the Arch package).

## Linked or compiled components

### libalpm

- **Purpose:** Read-only access to installed Arch package metadata.
- **Relationship:** Direct API use through dynamic linking. Moguet explicitly links with `-lalpm`, and the built ELF records `libalpm.so.16` as a `DT_NEEDED` entry on the audited system.
- **License:** `GPL-2.0-or-later`.
- **Provider:** The Arch Linux `pacman` package.
- **Bundled:** No. Moguet does not copy or distribute the libalpm source tree or library binary.

Moguet conservatively treats the dynamically linked result as a GPL-covered combined work and licenses the current project under `GPL-3.0-or-later`. libalpm remains limited to read-only metadata in Moguet's architecture; `pacman` remains the owner of system package transactions. That transaction ownership boundary does not change the license classification of direct library linking.

Because no libalpm source or binary copy is included in the Moguet source or package, Moguet does not install a duplicate GPLv2 text as a third-party copy. The providing Arch package and [pacman upstream](https://gitlab.archlinux.org/pacman/pacman) remain the sources for the library itself and its notices. Moguet's applicable GPLv3 text is installed as `LICENSE`.

### libcurl

- **Purpose:** HTTP requests to the AUR RPC service.
- **Relationship:** Direct API use through dynamic linking. Moguet explicitly links with `-lcurl`, and the built ELF records `libcurl.so.4` as a `DT_NEEDED` entry on the audited system.
- **License:** `curl` (SPDX identifier).
- **Provider:** The Arch Linux `curl` package.
- **Bundled:** No. Moguet does not copy or distribute the libcurl source tree or library binary.
- **Required notice:** [`LICENSES/curl.txt`](https://github.com/seekerkrt/moguet/blob/develop/LICENSES/curl.txt) in the source tree; `${PREFIX}/share/licenses/moguet/curl.txt` after installation (`/usr/share/licenses/moguet/curl.txt` in the Arch package).

`LICENSES/curl.txt` is an exact copy of the copyright and permission notice installed by Arch `curl` 8.21.0-1 and matches the [curl upstream license](https://curl.se/docs/copyright.html) for the audited release.

### nlohmann-json

- **Purpose:** Parse and serialize AUR RPC JSON data.
- **Relationship:** The system `<nlohmann/json.hpp>` header is used at build time. Its header-only implementation is compiled into the Moguet binary.
- **License for the compiled header closure:** MIT.
- **Provider:** The Arch Linux `nlohmann-json` package at build time.
- **Bundled source tree:** No.
- **Required notice:** [`LICENSES/nlohmann-json-MIT.txt`](https://github.com/seekerkrt/moguet/blob/develop/LICENSES/nlohmann-json-MIT.txt) in the source tree; `${PREFIX}/share/licenses/moguet/nlohmann-json-MIT.txt` after installation (`/usr/share/licenses/moguet/nlohmann-json-MIT.txt` in the Arch package).
- **UTF-8 decoder notice:** [`LICENSES/bjoern-hoehrmann-utf8-MIT.txt`](https://github.com/seekerkrt/moguet/blob/develop/LICENSES/bjoern-hoehrmann-utf8-MIT.txt) covers the attributed decoder code in the compiled header closure.

The audit used Arch `nlohmann-json` 3.12.0-2 and the matching upstream v3.12.0 tag. The C++20 include closure contained 46 nlohmann headers; every actual file carried `SPDX-License-Identifier: MIT`. The notice preserves the following copyright provenance found in compiled headers:

- Copyright (c) 2013-2025 Niels Lohmann
- Copyright (c) 2008-2009 Björn Hoehrmann
- Copyright (c) 2009 Florian Loitsch
- Copyright (c) 2016-2021 Evan Nemerson

The upstream README records the Hedley origin as CC0-1.0. The actual v3.12.0 `thirdparty/hedley/hedley.hpp` file compiled by Moguet carries the Evan Nemerson copyright and an MIT SPDX identifier, and it matches the installed Arch header. This provenance is retained here; Moguet does not bundle a separate Hedley source copy or rely on a separately redistributed CC0 work, so a CC0 full text is not installed.

`detail/meta/cpp_future.hpp` also contains an Apache-2.0-attributed Abseil fallback for pre-C++14 builds. Moguet is built as C++20, where that fallback branch is excluded from the preprocessed source. No Abseil fallback code or nlohmann upstream test/tool component is compiled or bundled, so an Apache-2.0 full text is not installed for the current build. Changing the C++ baseline, vendoring headers, or using an amalgamated header requires this decision to be re-audited.

### toml++

- **Purpose:** Parse TOML syntax for the typed user configuration model.
- **Relationship:** The system `<toml++/toml.hpp>` header is used at build time with toml++'s default header-only configuration. Its parser implementation is compiled into the Moguet binary. Moguet does not use the Arch package's shared-library mode or link with `-ltomlplusplus`.
- **License for the compiled header closure:** MIT.
- **Provider:** The Arch Linux `tomlplusplus` package at build time.
- **Bundled source tree:** No.
- **Required notice:** [`LICENSES/tomlplusplus-MIT.txt`](https://github.com/seekerkrt/moguet/blob/develop/LICENSES/tomlplusplus-MIT.txt) in the source tree; `${PREFIX}/share/licenses/moguet/tomlplusplus-MIT.txt` after installation (`/usr/share/licenses/moguet/tomlplusplus-MIT.txt` in the Arch package).
- **UTF-8 decoder notice:** [`LICENSES/bjoern-hoehrmann-utf8-MIT.txt`](https://github.com/seekerkrt/moguet/blob/develop/LICENSES/bjoern-hoehrmann-utf8-MIT.txt) in the source tree; `${PREFIX}/share/licenses/moguet/bjoern-hoehrmann-utf8-MIT.txt` after installation (`/usr/share/licenses/moguet/bjoern-hoehrmann-utf8-MIT.txt` in the Arch package).

The offline audit used Arch `tomlplusplus` 3.4.0-2 and the project's C++20 build flags. With no toml++ configuration macro defined, the effective configuration selected `TOML_HEADER_ONLY=1`, `TOML_IMPLEMENTATION=1`, `TOML_SHARED_LIB=0`, and `TOML_EXCEPTIONS=1`. The `<toml++/toml.hpp>` include closure contained 49 toml++ files. Forty-seven carried the MIT SPDX identifier and Mark Gillard notice; the reusable `header_start.hpp` / `header_end.hpp` implementation fragments carried no individual license marker. `unicode.hpp` records that its compiled UTF-8 decoder is based on Bjoern Hoehrmann's DFA decoder and retains his 2008-2009 copyright attribution. The separate Hoehrmann permission notice is preserved from the decoder's primary source.

`LICENSES/tomlplusplus-MIT.txt` is an exact copy of the MIT notice installed by the audited Arch package. A parse/link probe succeeded without `-ltomlplusplus`, and its ELF had no libtomlplusplus `DT_NEEDED` entry. Changing toml++ compile-mode macros, the exception or C++ mode, vendoring headers, or using a different package/header form requires this decision to be re-audited.

## External programs invoked

Every entry below is a separately installed program invoked through command-line arguments, stdin/stdout, and/or an exit status. It is not linked into Moguet and is not bundled with Moguet.

| Program | Purpose and provider | Process boundary | Bundled |
| --- | --- | --- | --- |
| `pacman` | Repository queries and system package transactions; Arch `pacman` package | Separate process; not linked | No |
| `pacman-conf` | Resolve pacman repository configuration and libalpm `RootDir`/`DBPath`; Arch `pacman` package | Separate process; not linked | No |
| `makepkg` | Build and install source packages; Arch `pacman` package | Separate process; not linked | No |
| `vercmp` | Compare Arch package versions; Arch `pacman` package | Separate process; not linked | No |
| `git` | Clone, inspect, fetch, diff, and update source repositories | Separate process; not linked | No |
| `bsdtar` | Read repository metadata from pacman sync databases and exact package-archive members; Arch `libarchive` package | Separate process; not linked | No |
| `sudo` | Enter the privilege boundary for privileged pacman operations | Separate process; not linked | No |
| `/bin/sh` | Execute constructed command lines. `printf` is used as a shell builtin on the audited system | Separate shell process; not linked | No |
| User-selected editor (default `nano`) | Review PKGBUILD, install scripts, and preference files; selected by the user or configuration | Separate process; no specific editor implementation is linked | No |

These programs are independently installed and distributed, and Moguet includes no copy of their executables or source. Their license texts are therefore not duplicated into the Moguet package. The program/package distributor remains responsible for the copies it provides.

Build tools such as the C++ compiler, `make`, `pkg-config`, GNU gettext tools (`xgettext`, `msgmerge`, and `msgfmt`), and the linker are also separate toolchain programs. They build Moguet and its compiled message catalogs but are not invoked by the production binary.

## System/toolchain runtime

On the audited Arch system, the Moguet ELF had direct `DT_NEEDED` entries for `libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`, and `libc.so.6` in addition to libcurl and libalpm. The C++ compiler driver supplies these system/toolchain runtimes; Moguet does not select them as application libraries or bundle their binaries.

`ldd` also expands dependencies required by system libcurl and libalpm packages, including protocol, TLS, compression, archive, and package-signing libraries. Those are transitive system dependencies, not direct Moguet API dependencies. The kernel-provided vDSO and ELF interpreter are likewise not third-party components bundled by Moguet. This notice intentionally does not turn every `ldd` line into a direct-component notice.

## Distribution notes

- The Moguet source tree contains no vendored third-party library source, library archive, shared library binary, or third-party test fixture.
- libalpm and libcurl are obtained as dynamic dependencies from Arch system packages. nlohmann-json and toml++ are obtained as system build dependencies.
- For Moguet v2.0.0 and later, the CMake install graph is the authority for the audited license and notice layout. `make install` is a frontend to that graph, and the Arch `PKGBUILD` consumes it directly through `cmake --install`. The current `PKGBUILD` describes only Moguet and does not evaluate historical jpacker versions or provide a legacy license-file fallback.
- External program license texts are not duplicated because no copy of those programs is distributed with Moguet.
- Vendoring, static linking, binary bundling, a new linked/compiled dependency, a different nlohmann-json or toml++ header form, or a project-built binary distribution requires a fresh notice and source-availability audit.
