# Issue #564 Slice 6: production recipe topologies

## Evidence and scope

These offline cases extend `tests/evaluated_devel_source_build_test.cpp` through
`tests/test-devel-tracking-bootstrap.py --topologies`. The default bootstrap
target includes them. They retain the production classifier, ordinary `-Syu`
coordinator, trial/confirmation, recipe review, exact acquisition, upstream
acceptance, native makepkg, S4, trusted install state machine, S5 and S6.
Existing transport seams redirect public Git/RPC to local fixtures and privileged
transaction effects to an isolated ALPM database. No Complete/provenance is injected.

Public recipe evidence was observed on 2026-09-16, before implementation:

| Recipe / exact AUR commit | Declared source / architecture | Packaging/source topology retained |
| --- | --- | --- |
| [tree-sitter-cli-git](https://aur.archlinux.org/cgit/aur.git/tree/?h=tree-sitter-cli-git&id=4522d1b9490417995b3d95741457eccccbc150c1) | One unaliased `git+https://github.com/tree-sitter/tree-sitter.git`; `i686`, `x86_64` | Root-only; binary under `docs/src/assets/images`; CLI input under `crates/cli` |
| [wezterm-git](https://aur.archlinux.org/cgit/aur.git/tree/?h=wezterm-git&id=33286bd9f22ed09224308d81991090a49185d175) | One `wezterm::git+https://github.com/wezterm/wezterm.git`; `x86_64`, `i686` | Four root Gitlinks plus freetype2 → dlg, logical name/path differences, recursive native preparation; binary icon/font/DLL; combined build inputs and packaged icon |
| [xpadneo-dkms-git](https://aur.archlinux.org/cgit/aur.git/tree/?h=xpadneo-dkms-git&id=992f1c3cac7428553de24eb16f26ea95fdea116c) | One `xpadneo::git+https://github.com/atar-axis/xpadneo.git`; `any` | Root-only; binary documentation image; upstream DKMS template transformed in prepare; module source/Makefile/config/modprobe/udev packaging |

All three are single-child PackageBases, with DefaultHead, no query or explicit
branch selector, no architecture-qualified source and no recipe-local supplemental
declaration. Wezterm's submodules are not additional `source=()` entries; xpadneo's
config inputs belong to the upstream tree. Do not invent multiple-source/split
features for these package names. Existing supplemental, pinned-branch, recursive
and split cases cover those independent contracts without triplicating them here.

The observed upstream commits are:

- tree-sitter: `1b8407d1e718f2a26e2886c03cc55622d8d1d7bd` (618 non-tree entries, 4,366,841 blob bytes).
- wezterm: `2658f629cd7251ce63a1698f238da2585676aa4e` (1856 entries, 169,049,536 blob bytes).
- xpadneo: `6988ca6b3e41703a4925488d2fd8a15709e20e80` (100 entries, 1,840,562 blob bytes).

Wezterm's `.gitmodules` and Gitlinks bind:

| Logical name | Path | HTTPS repository | Exact pin |
| --- | --- | --- | --- |
| harfbuzz/harfbuzz | deps/harfbuzz/harfbuzz | github.com/harfbuzz/harfbuzz.git | 33a3f8de60dcad7535f14f07d6710144548853ac |
| freetype/libpng | deps/freetype/libpng | github.com/glennrp/libpng.git | f5e92d76973a7a53f517579bc95d61483bf108c0 |
| deps/freetype/zlib | deps/freetype/zlib | github.com/madler/zlib.git | 51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf |
| freetype2 | deps/freetype/freetype2 | github.com/freetype/freetype2.git | 42608f77f20749dd6ddc9e0536788eaad70ea4b5 |

The exact freetype2 tree additionally pins logical `dlg`, path `subprojects/dlg`,
URL `https://github.com/nyorain/dlg.git`, commit
`72dfcc858c040c54a6a0b88fcb7e70ee186d3167`. That child is included in the fixture
and artifact input oracle. The pinned harfbuzz, libpng, zlib and dlg trees have
no further Gitlinks; all five observed child trees have no symlinks.

## Rejection and the owner decision

All three observed AUR HEAD responses contained two identical OID/HEAD records.
The #553 parser rejected these before reading metadata; Slice 1 normalizes exact
duplicates. Current declared source shapes meet the trial subset. Multiple
declared architectures are distinct from qualified sources and are supported by
Slice 2A. Current initial Missing still means RequiresCheck, not update proof.

The remaining Slice 6 blocker was mandatory whole-upstream text review. All three
actual trees contain NUL-bearing PNGs. Wezterm's 37,774,848-byte DLL exceeds the
old 8 MiB/blob limit; its root exceeds 32 MiB aggregate, and root plus the pinned
harfbuzz child exceed 4096 entries. A small text-only fixture would omit the real
failure class.

The adopted owner decision keeps recipe content review and separately authorizes
the exact upstream snapshot as build input using identity/topology metadata.
That acceptance is not a source-code safety certification. See the
[acceptance contract](../../docs/contracts/pinned-submodule-closure-review.md).

## Deterministic reduction and oracles

Package/Base names, URLs, OIDs and application payloads are fixture-owned. Git
objects and native makepkg/artifact bytes are real; all remote identity redirects
are confined to existing test transport hooks. No test downloads AUR or upstream.
The unaliased/aliased declaration, architecture order, single-child cardinality,
four root submodule paths/names, the nested dlg edge and prepared/package input use remain intact.

Binary samples carry NUL bytes. The wezterm case creates a real 37,774,848-byte
blob at the DLL path and 4100 child input files, crossing the old per-blob,
aggregate and entry review limits. Small local child repositories replace the
four application libraries and nested dlg; the existing sibling-reuse cases remain
independent. The artifact oracle checks all five child payloads and exact binary
icon bytes. Issue #589 adds a real annotated date tag, a later commit and the
WezTerm-shaped `git describe --long --tags --abbrev=7` / alphabetic-exclusion
`pkgver()` to this topology. It must succeed through native preparation and S4.
Issue #591 also makes native `prepare()` create a generic `SRCDEST/fixture-cache/`
with a small regular file. The preparation, prepared SRCINFO and packagelist
processes must exit zero; the retained mirror must then pass prepared/post-build
proof and exactly one package build through S6 Complete. No live Cargo fetch is
needed to retain this auxiliary-cache topology. The recursive negative lane and
root-tag workspace lane also keep this sibling during execution; the latter
adds mirror missing/replacement/symlink/config/remote/HEAD/alternate rejection
and verifies that identity substitution still refuses cleanup.
The root-tag workspace lane separately compares plain and pinned dynamic versions,
lightweight/nested/non-reachable/empty/SHA-256 tags, and both stores at all four
phase boundaries. It renames the fixture remote offline after acceptance so
native child Git cannot silently use the fixture `insteadOf` acquisition route.
DKMS checks the evaluated template, module source and installed
modprobe/udev paths. The CLI case checks its selected upstream input in the archive.
Its native `prepare()` writes a real split Git commit-graph and `build()` writes
the single-file form. Both reproof points must accept these derived indexes;
the test checks real makepkg exit zero through S4, the reviewed execution owner,
snapshot, executor and S6. This reproduces the tree-sitter bootstrap rejection
without relying on the size-dependent auto-maintenance threshold.

Cargo registry downloads, upstream application compilation and kernel DKMS
execution are not exercised. These tests prove Moguet's source topology and
authority route, not a live build of those applications. The actual source
transport/resource budgets remain independent of the removed text-review limits.

All cases share the existing migration oracle: initial RequiresCheck/Missing,
explicit migration, full recipe review, separate snapshot acceptance, common S4,
exact install/S5 and S6 Complete/readback. Same remote is UpToDate, the second
ordinary update uses the production presenter with warning/prompt/mutation zero
and normal success, and advancing the root gives UpdateAvailable(GitRevision).
Publication bytes and phase counters are unchanged on the second invocation.

## Existing review cases under the new contract

- Binary: becomes a positive exact-identity acceptance case.
- Blob/aggregate/line content quotas: superseded by the representative real large
  binary and inventory case; no obsolete quota override remains.
- Empty/no-newline/content escaping cases: whole-blob rendering no longer exists;
  recipe and terminal-safe rendering owners retain their own tests.
- Read failure/cancel/cleanup during review: review performs zero blob reads;
  4A acquisition/read taxonomy and cleanup tests remain in their owner lane.
- Symlink, metadata entry/render limits, output failure, explicit acceptance,
  non-TTY/noconfirm/nodiff, cancellation, ownership and cleanup: rejection/lifetime
  coverage remains. No new negative matrix is added.
