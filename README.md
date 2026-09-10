# Moguet

[日本語](README.ja.md)

<!-- parity:overview -->
## Overview

Moguet is a pacman-first AUR helper for Arch Linux with verified source
builds and per-package build preferences. It keeps package transactions with
`pacman`, package builds with `makepkg`, and repository transport with `git`,
while Moguet owns planning, review, artifact validation, and the safe hand-off
between those tools.

Moguet is not an official Arch Linux, pacman, or AUR project. It is not an
independent package manager, a complete clone of another AUR helper, or a
replacement for the contracts of pacman and makepkg.

<!-- parity:name -->
## Name and identity

The project and brand name is **Moguet**, pronounced **モグエット**. The
command, binary, package, and XDG application name is
`moguet`; environment variables owned by the project use the `MOGUET_*`
prefix.

The name starts from the Japanese expression *mogu mogu* (もぐもぐ) and the
English diminutive suffix *-let*. The resulting *Mogulet* was shaped toward
French *muguet*, meaning lily of the valley. The flower is small and modest
but poisonous; that combination reflects a small helper that warns about
danger or ambiguity and stops before external mutation when it cannot make an
authoritative decision.

Moguet is a project-specific coined name. Its formal project spelling is
**Moguet**, and its formal Japanese reading is **モグエット**.

<!-- parity:status -->
## Project status

Moguet v2.0.0 is a breaking identity, storage, configuration, localization,
and packaging transition built on the jpacker v1.16.0 execution base. The
local `moguet` binary, XDG paths, typed TOML configuration, and gettext-based
English/Japanese CLI surface are implemented. The local package identity,
payload, dependency metadata, documentation, and non-destructive transition
from jpacker v1.16.0 form the v2 release contract.

Moguet v2.0.1 completes the source-preference part of that adopted XDG storage
contract. It corrects an implementation omission in v2.0.0 rather than adding
a new storage direction: source-build preferences now use only the executing
user's XDG config context, while the published v2.0.0 tag, Release, and release
notes remain historical records.

Moguet v2.7.0 is the latest release. It adds authoritative Git revision tracking
for supported devel packages with verified build/install provenance, including
same-version updates, and distinguishes unavailable package metadata from
confirmed absence. The initial Git/HTTPS subset and explicit source-review
requirements remain narrow; this is not full VCS tracking. See the
[v2.7.0 release](https://github.com/seekerkrt/moguet/releases/tag/v2.7.0)
for the complete user-visible changes.

The canonical repository identity is Moguet on GitHub, with a GitLab mirror.
The Moguet package does not provide a `jpacker` command alias. AUR publication
is a separate future decision; this document does not claim that an AUR
endpoint exists.

Moguet v2.x is published and usable, but it remains a development-phase
product rather than a finished, general-purpose AUR helper. Basic pacman
wrapping, AUR source builds, updates, and per-package source-build
preferences already work today, while the wider AUR-support surface and
edge-case coverage are still being implemented incrementally and the UX is
still maturing. Moguet remains pacman-first rather than reimplementing a full
dependency solver or automatic provider/conflict resolution, and does not
promise the same automatic-resolution completeness as established AUR
helpers: unsupported or ambiguous cases stop fail-closed instead of guessing.
v2.x is the public development period that
builds Moguet's source-aware entry points, safety boundaries, and validation
infrastructure; v3.0.0 is the point where Moguet-specific build-profile and
PKGBUILD-diff workflows come together, which the project treats internally
as Moguet's full commissioning. See the release roadmap
([issue #344](https://github.com/seekerkrt/moguet/issues/344)) for the
detailed plan.

<!-- parity:safety -->
## Design and safety boundaries

- Run `moguet` as a normal user. It invokes `sudo pacman` only for operations
  that require a system package transaction; AUR source retrieval, review, and
  builds do not run as root.
- `pacman` and libalpm remain the authorities for package database state and
  package transactions. `makepkg` builds packages, and `git` retrieves AUR
  repositories. Moguet does not reimplement those tools.
- `deps` and `plan` only inspect and present information. They do not clone,
  build, or install. `fetch` clones missing repositories or runs only
  `git fetch origin` for an existing clone; it does not pull, merge, reset,
  advance the working tree, build, or install.
- Repository metadata failure is distinct from confirmed absence. Auto `-Si`
  falls back to AUR only after confirmed absence; a failed target is reported
  and excluded while independent targets continue. `revert` checks repository
  metadata before deleting each preference, preserving failed targets and
  grouping successful repository targets into the existing reinstall.
- AUR info shows an unavailable installed state with a warning instead of
  reporting `no`. Auto search derives `[installed]` only from a successful
  foreign inventory; unavailable inventory produces a warning and omits the
  annotation. These optional annotations do not change the search/info success
  policy. AUR-only search does not query installed or repository metadata.
- Dependency edges retain the typed requirement, source-aware candidate, and
  constraint result. `deps` continues with a warning for `Unsatisfied` or
  `Unknown`; `plan` marks the result incomplete. `Invalid` and `Conflicting`
  fail closed. `fetch`, build, install, upgrade, and local build stop before
  clone, fetch, source mutation, build, sudo, pacman, or transaction work when
  the result is `Unsatisfied` or `Unknown`.
- AUR `Conflicts` and `Replaces` declarations are assessed before build and
  install against installed and planned packages, including provided
  components and versioned relations. Diagnostics distinguish an installed
  conflict from a planned-target conflict and present a matching replacement
  only as a potential impact that requires review. Moguet does not remove a
  package, select a replacement target, or resolve a conflict automatically.
- An unavailable (`Unknown`) or invalid relation assessment fails closed; it
  is never presented as an absence. Only a complete observation that confirms
  no matching current or planned package or provided component releases that
  relation guard. The declaration still exists, and pacman/libalpm remains the
  transaction authority. `-Si` shows the source metadata and explicitly
  defers this stateful assessment to planning and build preflight.
- When multiple provider candidates remain, an interactive TTY lists
  source-aware candidates by number and requires exactly one explicit choice;
  there is no default. Empty input, `q`, `quit`, `cancel`, or EOF cancels the
  choice, while invalid or out-of-range input retries.
- An interactive provider list appends localized `[installed]` when the
  candidate's package name exists in the read-only local package database.
  It leaves not-installed candidates untagged and marks an unavailable lookup
  as `[installed state unknown]` with a warning. This name-only observation
  neither proves provenance or version compatibility nor changes candidate
  order, numbering, or the required explicit choice.
- Non-TTY use and `--noconfirm` do not read provider input from stdin or
  auto-select a candidate. Unselected ambiguity fails closed.
- `moguet -S --select [--needed] <query>` discovers source-aware root package candidates
  from official repositories and AUR. An interactive TTY accepts package
  numbers, multiple numbers, inclusive ranges, and an `@group` selector for a
  displayed official group; there is no default, even for one candidate.
  Empty input, `q`, `quit`, `cancel`, or EOF cancels, and invalid input retries
  against the same candidate list.
- Root package discovery does not query candidates or prompt on non-TTY stdin
  or with `--noconfirm`. After selection and static preflight finish, selected
  repository roots run first in one exact `repository/package` transaction;
  only its success allows selected AUR roots to enter the existing source-build
  lifecycle. A later AUR failure does not roll back the completed repository
  transaction.
- Provider choices belong only to the current invocation. `deps` and `plan`
  distinguish selected and ambiguous providers; a selected AUR provider's
  PackageBase flows into fetch/build planning, while a selected repository
  provider is introduced as an official `repository/package` dependency. In
  `deps --recursive`, among provided dependencies, only a user-selected AUR
  provider is traversed; unique providers and selected repository providers
  remain terminal.
- Constraint results do not filter, sort, number, recommend, default, or
  auto-select provider candidates, and do not authorize source fallback.
  When selected AUR provider metadata is refreshed, the current matching
  capability is evaluated again and the stale result is discarded.
- Source-build routes keep their route-specific lifecycle. A standalone
  official-repository build uses `PackageBaseSet`; a registered official
  source uses `PackageBaseSet` after `OnlyIfUpdated` preparation. The sync
  repository route remains `SingularCompatibility` with its existing
  `--needed` behavior, while a registered AUR source remains
  `SingularCompatibility` with its existing provider and split-package guards.
- Official-repository source identity comes from the exact configured libalpm
  snapshot. Only a confirmed `NotFound` may fall back to AUR; query,
  configuration, or metadata failure stops. Standalone and registered builds
  build the authoritative PackageBase once, install only the requested
  `Explicit` child, and retain sibling and debug outputs as unselected and not
  installed.
- The registered AUR route offers provider selection only when every candidate
  is from an official repository. A candidate set containing an AUR provider
  remains ambiguous and stops before system or source execution because this
  singular compatibility route cannot schedule that provider's PackageBase.
- Moguet rejects unresolved dependencies, unselected ambiguous providers,
  cycles, blocking conflict/replacement assessments, and unprovable artifact
  identities before the corresponding mutation.
- `--noconfirm` avoids interactive blocking. It is not “yes to everything” and
  does not bypass source selection, planning, identity, conflict, or ownership
  guards, and it never authorizes automatic removal or replacement.
- Moguet-owned boolean confirmations use `[Y/n]` for a Yes default, `[y/N]`
  for a No default, and `[y/n]` for no default. They accept the fixed
  case-insensitive ASCII tokens `y` / `yes`, `n` / `no`, and `q` / `quit` /
  `cancel`. Enter at `[y/n]` warns and re-prompts instead of approving.
  `--noconfirm` uses only a declared default, while non-TTY input may use only
  a safe No; neither turns an approval-required no-default prompt into Yes.
  A question-specific decline and cancellation of the current operation are
  distinct from each other and from actual input, command, or internal
  failure. Cancellation stops later work but does not roll back completed
  phases. See the [interactive confirmation contract](https://github.com/seekerkrt/moguet/blob/develop/docs/contracts/interactive-confirmation.md).
- Multi-phase upgrades are not one atomic transaction. A failure stops later
  work but does not roll back an already completed package transaction. If
  cleanup fails after installation succeeds, inspect the result before
  retrying; the package may already be installed. In `upgrade-all`, provider
  selection for the filtered AUR phase occurs before clone, build, pacman, or
  sudo work in that phase, but earlier phases may already have completed.
- Exact target-less `moguet -Syu` is also sequential: it completes the official
  repository system upgrade first, then obtains a fresh installed-foreign/AUR
  inventory and performs the normal AUR update. A repository failure leaves
  the AUR phase unattempted. A blocker, execution failure, or cleanup failure
  after repository completion is reported as a non-zero partial outcome; the
  completed repository transaction is not rolled back.

The detailed compatibility and routing contract is in
[docs/COMPATIBILITY.md](https://github.com/seekerkrt/moguet/blob/develop/docs/COMPATIBILITY.md),
and adopted design decisions are recorded in
[docs/DECISIONS.md](https://github.com/seekerkrt/moguet/blob/develop/docs/DECISIONS.md).

<!-- parity:installation -->
## Installation

### Build requirements

- An Arch build environment with `base-devel` preinstalled; its current
  members provide the C++ toolchain, `pkgconf`, and GNU gettext development
  tools
- `cmake` 3.18 or later; the optional tracked developer preset requires 3.19
  or later
- `pacman`, `pacman-conf`, and libalpm development metadata
- `git`
- `curl`
- `nlohmann-json`
- `tomlplusplus`

Build and inspect the development tree with:

```bash
git clone https://github.com/seekerkrt/moguet.git
cd moguet
make
./moguet --help
```

CMake is the authority for C++ build and install targets, CTest is the
authority for C++ test registration and execution, and the root `Makefile`
keeps the familiar developer shortcuts plus repository-specific validation.
Plain `make` uses `build/cmake-production` with `BUILD_TESTING=OFF`; `make
test` uses `build/cmake-testing` with `BUILD_TESTING=ON`, runs CTest, and then
runs the repository validation layer. Existing focused entries remain
available as `make test-<area>`.

For a repeatable debug/editor configuration, use the tracked developer
preset through its post-success frontend:

```bash
make cmake-dev-configure
cmake --build build/cmake-testing
ctest --test-dir build/cmake-testing --output-on-failure
```

This preset enables `BUILD_TESTING=ON`, a Debug build, and
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`. It exposes that tree's database through
the generated repository-root `compile_commands.json` symlink for clangd and
other editor or analysis tools. `make cmake-dev-configure` runs `cmake
--preset dev-debug` and publishes the link only after the complete configure
and generate process exits successfully. A failed configure or generate
preserves the previous valid publication and publishes nothing on a first
failure. Raw `cmake --preset dev-debug` remains a configure-only CMake entry
and does not publish the root link. `make clean` removes both wrapper-owned
build trees and the root link. CMake Presets require CMake 3.19 or later. The
project remains directly configurable with its declared CMake 3.18 minimum
when the optional preset CLI is not used.

For a packaging-safe dry run, stage the payload outside the live filesystem:

```bash
stage_dir=$(mktemp -d)
make PREFIX=/usr DESTDIR="$stage_dir" install
find "$stage_dir" -type f -print
```

`make install` and `make uninstall` are frontends to the canonical CMake
install graph and its exact `install_manifest.txt`; the destination overrides
shown above are mapped into that graph rather than implemented by a separate
Make install recipe.

The current development package also installs the private implementation
helpers `/usr/libexec/moguet/moguet-alpm-receipt-helper` and
`/usr/libexec/moguet/moguet-source-artifact-install-helper`. They are separate
package-owned root transaction authorities, not public commands: they are
outside `PATH`, have no man pages, do not accept an executable, state root, or
destination path, and must never be replaced by a helper from a source or
build tree. The source-artifact helper stages only write-sealed validated
artifact bytes into its private root-owned transaction state before a fixed
`pacman -U` hand-off. The current public source-build `--rmdeps` route remains
unsupported and fail-closed; installing either helper does not enable
dependency cleanup.

The current development tree uses those owner-specific helpers inside one
closed remote-AUR lifecycle to build an internal cleanup-candidate assessment.
Only an exact, correlated actual dependency `Install` can become internally
eligible after the full invocation and current metadata/policy observations
succeed. This assessment has no public preview, prompt, or removal connection;
makepkg sync-dependency ownership remains a separate unresolved authority.

The v2.0.0 package and its only executable are named `moguet`; it does not
install `/usr/bin/jpacker`. Its payload is disjoint from the jpacker v1.16.0
package, so the metadata intentionally declares no `provides`, `conflicts`, or
`replaces` relationship with `jpacker`. The packages may coexist while the
manual migration and rollback are verified. Their source-preference stores are
separate: Moguet uses its user-owned XDG config authority and does not read or
modify the legacy `/etc/jpacker/package.build/` store. The Moguet package
creates or owns neither user XDG data nor the legacy directory.

The package runtime dependencies are `curl`, `git`, `libalpm.so`, `libarchive`,
`nano`, `pacman`, and `sudo`. The exact `makedepends` set recorded by the
package is `cmake`, `nlohmann-json`, and `tomlplusplus`. Arch package builds assume
`base-devel` is preinstalled; its current membership supplies GNU gettext and
`pkgconf`, so `base-devel`, `gettext`, and `pkgconf` are not listed in
`makedepends`. `git` remains a runtime dependency and is not duplicated there.
gettext supplies the catalog build tools; the runtime binary has no separate
libintl dependency.

Moguet v2.0.0 does not include AUR publication. Do not invent an AUR URL or
install a development payload on the live system. See the
[v1 to v2 Migration Guide](docs/migration/v1-to-v2.md) before changing an
installed system.

### Package installation with `PKGBUILD`

The repository root ships a `PKGBUILD` that packages a published release
rather than the checked-out working tree. Its `pkgver()` reads the version
from the repository's own `VERSION` file and fetches the matching published
Git tag (`v<version>`) as the build source, so it never packages unreleased
working-tree commits.

```bash
git clone https://github.com/seekerkrt/moguet.git
cd moguet
makepkg -si
```

`makepkg -si` builds that tagged release and installs it onto the live
system with `pacman -U` in the same step. This differs from `make` and
`./moguet --help` above, which only build and inspect the development tree
in place and install nothing. The `PKGBUILD` is the canonical production
CMake build/install consumer and configures `BUILD_TESTING=OFF`; the 115
developer C++ test-ledger executables, one `EXCLUDE_FROM_ALL` installed
transport fixture harness, and 146 CTest registrations remain in host, CI,
and release validation. This `PKGBUILD` is a repository-provided
packaging path, not an AUR submission; Moguet still has no published AUR
page.

<!-- parity:usage -->
## Basic usage

`moguet --help` is the runtime authority for the current command and option
surface. Command and option tokens never change with the selected locale.

The closed Moguet-owned and intercepted grammar is:

<!-- CLI CANONICAL GRAMMAR BEGIN -->
```text
build <pkg> [V=K...]
build --local <directory> [V=K...]
upgrade
upgrade-aur
upgrade-all
clean
deps [--recursive] <pkg>...
plan <pkg>...
fetch <pkg>...
add-src <item>...
edit-src <pkg>...
list-src
del-src <pkg>...
revert <pkg>...
-G <pkg> [--output-dir=DIR]
-Gp <pkg>
-S --select [--needed] <query>
-Syu [--needed]
-Syu --repo [--needed]
```
<!-- CLI CANONICAL GRAMMAR END -->

The two exact target-less `-Syu` forms are Moguet-intercepted semantic routes;
the repository-only form still accepts a compatible delegated pacman tail.
Other pacman operation forms remain delegated open grammar, not a Moguet
allowlist. The closed grammar rejects a second bare operand for remote or local
`build`, and rejects target operands for `upgrade`, `upgrade-aur`,
`upgrade-all`, `clean`, and `list-src`. Inspection and source-maintenance forms
shown with `...` keep their multi-target behavior.

```bash
# Install, search, or inspect packages
moguet -S <pkg>
moguet -S --select [--needed] <query>
moguet -Ss <query>
moguet -Si <pkg>

# Ordinary AUR-helper update: official repositories, then normal installed AUR
moguet -Syu

# Repository-only system upgrade
moguet -Syu --repo

# Update configured source packages, installed AUR packages, or both
moguet upgrade
moguet upgrade-aur
moguet upgrade-all

# Build and install one remote package or one local PKGBUILD root
moguet build <pkg> [V=K...]
moguet build --local <directory> [V=K...]

# Inspect AUR dependencies and build order without building
moguet deps --recursive <pkg>...
moguet plan <pkg>...

# Retrieve build repositories without building or installing
moguet fetch <pkg>...

# Maintain one or more source-build preferences
moguet add-src <item>...
moguet edit-src <pkg>...
moguet list-src
moguet del-src <pkg>...
moguet revert <pkg>...

# Export one PackageBase checkout or print only its PKGBUILD
moguet -G wezterm-git
moguet -G wezterm-git --output-dir=./exports
moguet -G wezterm-git --output-dir="$HOME/src/aur"
moguet -G wezterm-git --output-dir=/home/user/src/aur
moguet -Gp <pkg>

# Observe every supported mutating route without changing persistent state
moguet --dry-run -S <pkg>
moguet --dry-run -Syu
moguet --dry-run -Syu --repo
moguet --dry-run fetch <pkg>...
moguet --dry-run build <pkg>
moguet --dry-run build --local <directory>
moguet --dry-run upgrade
moguet --dry-run upgrade-aur
moguet --dry-run upgrade-all
```

`-G` exports to the command-start current directory when `--output-dir` is
omitted. `--output-dir=DIR` selects an existing parent directory for that
invocation only; a relative value is resolved from the command-start current
directory, and the validated PackageBase remains the direct-child destination
name. Moguet does not create the parent, follow any symlink component in the
specified path, overwrite an existing destination, or provide its own tilde
expansion. Use `--output-dir="$HOME/src/aur"` or an absolute path instead of
`--output-dir=~/src/aur`. The option is not supported by `-Gp` or other
operations, and only the attached form is accepted.

`--dry-run` is a global observation modifier for the Moguet-owned `-S` install
and system-update routes, `fetch`, remote and local `build`, `upgrade`,
`upgrade-aur`, and `upgrade-all`. It explicitly rejects `deps`, `plan`, `-Ss`,
`-Si`, `clean`, and generic pacman pass-through routes instead of forwarding
the option to pacman. The renderer reports `Ready` or `NoOp` with exit status
0 and `Blocked` with a non-zero status.

Dry-run can perform the same read-only filesystem, network, and exact
allowlisted pacman discovery queries as the production preflight, but does not
write the state log or persistent state, create a cache, workspace, or
worktree, run Git clone/fetch/checkout mutation, run `makepkg --printsrcinfo`
or other local metadata evaluation, produce build output, invoke sudo, start
or mutate through a pacman transaction, acquire a pacman transaction lock,
install packages, or perform cleanup mutation. A local build that needs
metadata evaluation is therefore `Blocked`; it is never guessed ready. The observation
is not reused as an approval token, execution capability, or cached provider
choice: a later actual invocation revalidates current state. The v2.2.0 surface
is human-readable only and adds no JSON or other machine-readable plan schema.

For exact target-less `moguet --dry-run -Syu`, the repository system-update
intent and the later normal-AUR transaction intents are shown separately. The
AUR assessment is based on the currently installed state, not the state after
a hypothetical repository transaction. Actual `moguet -Syu` does not reuse
that observation or any prepared capability: after the repository upgrade
succeeds it obtains a fresh installed-foreign inventory, AUR metadata, plan,
provider decisions, and preflight. `moguet --dry-run -Syu --repo` shows only
the repository intent and does not query AUR or source-build preferences.

Human-readable diagnostics are projections of typed state, never the authority
used to classify it. English and Japanese keep the same hierarchy: a normal
summary first, attention-required details next, and route-owned necessary
detail last. Operation outcome and package-state observation remain distinct;
plan construction, completeness, and execution readiness are reported
independently. A successful but unverified observation remains successful with
the required check, `Unknown` is not rewritten as `NoOp`, and severity,
blocking, and exit-status effect remain separate dimensions.

**Choosing an upgrade command:** Use exact target-less `moguet -Syu` for the
ordinary AUR-helper update: it completes the official repository system
upgrade, then re-evaluates the installed foreign/AUR state and performs a
normal AUR update. It neither enumerates nor reads saved source-build
preferences, including a PackageBase fallback preference, and preference data
is not applied to the AUR work environment. A valid, invalid, unreadable, or
otherwise failing saved preference therefore does not affect this route.

The explicit `upgrade*` commands are Moguet source-aware workflows and keep
strict saved-preference handling. `upgrade` performs the repository system
update plus configured source-build preferences. `upgrade-aur` updates only
installed AUR packages (it does not run the repository system update), while
still checking and applying saved source-build preferences for those AUR
targets. `upgrade-all` performs the repository update, configured-source
lifecycle, and remaining AUR update. These commands are not aliases for
ordinary `-Syu`.

Only the exact target-less canonical `-Syu` token enters the combined route.
`-Sy`, `-Su`, alternate or separated modifier spellings, target-bearing
`-Syu <pkg>`, and unknown modifier forms retain their existing routing and do
not start an installed-AUR sweep. Initially, `--needed` is the only pacman
semantic option supported by automatic combined `-Syu`; it applies only to
the repository transaction. Any other pacman semantic option or unsupported
argument form fails before repository mutation, with guidance to use
`moguet -Syu --repo`. The repository-only form removes the semantic selector
before invoking pacman, preserves the compatible pacman pass-through surface,
and performs no AUR inventory, AUR RPC, preference, cache, Git, or makepkg
work. `moguet -Syu --aur` is unsupported; use the source-aware
`moguet upgrade-aur` for an AUR-only update. `--noconfirm` never bypasses
provider, conflict/replacement, required `RequiresCheck`, or other safety
guards, and it never approves an unverified devel update.

Moguet v2.5.0 treats installed exact-AUR packages with conventional `-git`,
`-svn`, `-hg`, `-bzr`, `-cvs`, or `-darcs` suffix evidence as devel
candidates. When the normal AUR version is not newer and only that suffix
evidence is available, `moguet -Qua` reports `RequiresCheck` with the package,
PackageBase/child evidence, and reason instead of silently treating the
package as up to date. A normal newer AUR version remains an update candidate
and keeps the existing precedence.

`RequiresCheck` is neither `UpToDate` nor an automatic rebuild decision. In
ordinary exact target-less `-Syu`, an independent target in this state is
skipped with a non-blocking warning; unrelated normal AUR updates can continue,
and the aggregate can succeed. Its update status remains unverified. If another
update candidate requires that target through a dependency, provider, or
required-child relation, the affected root remains blocked and the operation
is non-zero. `upgrade-aur`, its dry-run, and the fresh AUR phase of
`upgrade-all` retain their strict whole-operation blocker behavior. Non-TTY use
and `--noconfirm` do not add a prompt or approve a rebuild.

v2.5.0 does not query or compare the upstream VCS revision and does not publish
devel build provenance. Moguet now includes the trusted
HTTPS Git remote revision observer foundation from
[issue #475](https://github.com/seekerkrt/moguet/issues/475), limited to
default HEAD and exact branches with strict complete SHA-1 / SHA-256 results.
The internal [issue #476](https://github.com/seekerkrt/moguet/issues/476)
Slice 7-B coordinator compares the provenance tip with fresh installed and
reviewed state before querying the remote, then rechecks local state before
returning `UpdateAvailable` / `UpToDate`. Slice 7-D connects this producer to
normal AUR updates, registered AUR updates, `-Qua`, and affected dry-run routes.
A newer normal AUR version keeps precedence without a Git query; same/older
versions may be refined by the validated Git assessment. Git revision differences
are displayed separately from package-version changes.

The initial authoritative execution path requires a real reviewed pin and the
existing single-child HTTPS Git subset. It consumes S4/S5/S6 once, with no legacy
fallback after starting. Installation success and provenance publication failure
remain separate partial outcomes and produce a non-zero result. Registered
`RequiresCheck` uses an explicit default-No rebuild confirmation; this does not
replace source review. `--noconfirm` cannot supply review authority. See the
[normal devel route contract](https://github.com/seekerkrt/moguet/blob/develop/docs/contracts/devel-normal-routes.md).
The provenance format remains schema v1 in a separate XDG state namespace.
Unknown/future schemas and corrupt or unsafe history fail closed; the updater does
not repair records, adopt external history, or create a missing baseline. A baseline
requires an explicitly reviewed supported build, an actual install, and successful
publication. Same-version reinstalls invalidate historical provenance when the
installed artifact binding changes. See the [devel tracking and migration contract](https://github.com/seekerkrt/moguet/blob/develop/docs/contracts/devel-tracking.md).

`--aur` limits supported `-S`, `-Ss`, and `-Si` forms to AUR. `--repo`
limits those forms to official binary repositories and is also the
repository-only selector for exact target-less `-Syu`. `--aur` is not accepted
with `-Syu`. Combining the selectors is an error before an external command or
AUR query. Pacman-only routes preserve compatible pacman options; a
source-build route rejects options whose meaning cannot be preserved instead
of silently ignoring them.

`-S --select [--needed] <query>` is the interactive source-aware discovery form. Without
a source selector it searches both official repositories and AUR; `--aur` or
`--repo` limits the candidate source. Only `--needed` has a shared meaning on
both selected routes. Non-TTY use and `--noconfirm` fail without querying or
choosing a package.

Source-build preferences are managed with multi-target `add-src`, `edit-src`,
`del-src`, and `revert`, plus target-less `list-src`. A one-off
`build <pkg> [V=K...]` resolves a remote package and does not save a preference.

### Reviewed AUR source workflow

For an AUR Git source build, Moguet keeps the last explicitly accepted exact
upstream commit for each PackageBase in persistent XDG state. After fetch or
clone, it pins one exact target commit. With no reviewed state—including an
existing cache created before this workflow—the first affected PackageBase
gets a full tracked-file review. A later target is reviewed from the previous
reviewed revision; the same target needs no new prompt or state write. If the
old commit object is unavailable, Moguet presents a full rebaseline review
instead of falling back to the cache checkout. Invalid, corrupted, or
source-mismatched state requires an explicit full rebind review; future or
unsafe state fails closed.

The review inventory covers every tracked added, modified, deleted, renamed,
or type-changed file, not an extension allowlist. Root `PKGBUILD` and top-level
`*.install` files receive review-sensitive guidance, while patches, service
units, helper scripts, local source/config files, binary changes, and other
tracked content remain visible. `.SRCINFO` stays visible as lower-priority
generated metadata and is not a substitute for source review.

`--diff` selects the reviewed-source prompt policy, but only an explicit
interactive `y` or `yes` after a complete review advances the stored revision.
`--nodiff`, a review decline, `--noconfirm`, or non-TTY input may retain the
existing compatibility build behavior, but they do not advance reviewed
state. Cancellation, EOF, input failure, an unsupported review, and unsafe or
future state stop without advancing it.

An accepted build is checked out at the exact target commit; mutable checkout
HEAD, branches, and remote refs are not build authority. Publication uses a
compare-and-swap guard so a concurrent review is not overwritten, and a later
build or install failure does not roll back a correctly accepted revision.
`--edit` / `--noedit` control invocation-local PKGBUILD and `.install` editing,
not upstream acceptance: editor changes are a separate overlay on the reviewed
commit. Official-repository and `build --local` routes do not create this
state. See the [reviewed AUR source state contract](https://github.com/seekerkrt/moguet/blob/develop/docs/contracts/reviewed-source-state.md).

### Per-package build customization

The existing v2.x `[V=K...]` and source-build preference forms let you adjust
one package's build environment without editing the system-wide
`/etc/makepkg.conf`. Complete replacement and inheriting the system values
before a local modification are separate patterns; choose between them for the
specific task rather than treating either one as a universal default.

For a one-off complete override:

```bash
moguet build example-package \
  CFLAGS="-O3 -pipe" \
  CXXFLAGS="-O3 -pipe"
```

Each named variable is set to an explicit value for this package build; it is
not merged with the corresponding system-wide value. This can be useful when
deliberately simplifying flags for troubleshooting or trying a package-specific
setting. `-O3` is illustrative here, not a performance recommendation.

To inherit the current system settings and modify only one part:

```bash
source /etc/makepkg.conf

moguet build obs-studio \
  CFLAGS="${CFLAGS/-O2/-O3}" \
  CXXFLAGS="${CXXFLAGS/-O2/-O3}"
```

Here, `source` loads `/etc/makepkg.conf` into the current shell as a baseline;
it does not modify the file. Each substitution preserves the other existing
flags and changes a matching `-O2`; if that text is absent, the value remains
unchanged. This is not a universal optimization recipe, and `-O3` remains an
illustrative value.

A mixed C/C++ package may need both `CFLAGS` and `CXXFLAGS`, while a C-only or
C++-only package may need different variables. The package's `PKGBUILD` and
upstream build system determine which environment flags they consume; Moguet
does not guarantee that these variables affect the compiler invocation.

`build` remains a one-off operation and does not save these assignments. After
verifying a setting, use `add-src` to save it as that package's source-build
preference. Save a complete override with:

```bash
moguet add-src example-package \
  CFLAGS="-O3 -pipe" \
  CXXFLAGS="-O3 -pipe"
```

Or save the result of inheriting and locally modifying the system values:

```bash
source /etc/makepkg.conf

moguet add-src obs-studio \
  CFLAGS="${CFLAGS/-O2/-O3}" \
  CXXFLAGS="${CXXFLAGS/-O2/-O3}"
```

`build --local <directory> [V=K...]`
instead treats exactly one user-owned directory as a local PackageBase source;
it does not infer a local root from a path-like package operand or query AUR for
that root.

The local route reads a safe `.SRCINFO` without modifying it. Missing, invalid,
or known-stale metadata requires PKGBUILD review and explicit no-default consent
before `makepkg --printsrcinfo`; non-TTY input and `--noconfirm` stop instead of
authorizing evaluation. Moguet builds from an invocation-owned source snapshot,
leaves the user-owned tree unchanged, and installs every valid unique `pkgname`
child declared by the accepted metadata as an explicit root. Dependency
artifacts retain dependency install reasons, and an already explicit installed
package is never demoted. Runtime-aware package-name completion and more
advanced completion are future work; the shipped completion is limited to the
public CLI schema.

<!-- parity:configuration -->
## Configuration

Moguet reads one optional, user-owned TOML file:

```text
$XDG_CONFIG_HOME/moguet/config.toml
fallback: ~/.config/moguet/config.toml
```

In a repository checkout, the canonical copy is `sample/config.toml`. A
standard Arch package installation places the same file at
`/usr/share/doc/moguet/examples/config.toml`. Copy it to
`$XDG_CONFIG_HOME/moguet/config.toml`, or to the fallback shown above only when
`XDG_CONFIG_HOME` is unset, before editing it. The enum values `prompt`, `skip`,
`normal`, `rebuild`, and `clean` are TOML strings and must remain quoted; the
canonical sample uses double quotes.

The minimal v2.0.0 schema and built-in defaults are:

```toml
schema_version = 1

[review]
pkgbuild = "prompt"
diff = "prompt"

[build]
mode = "normal"
```

Values are composed in this order:

```text
built-in defaults -> user config -> CLI override
```

The canonical CLI overrides are `--edit` / `--noedit`, `--diff` /
`--nodiff`, and `--build-mode=normal|rebuild|clean`. `--rebuild` and
`--cleanbuild` are compatibility aliases for the corresponding build modes.
Conflicting overrides fail before external mutation rather than using
last-one-wins behavior.

`review.diff` and `--diff` / `--nodiff` select the AUR reviewed-source prompt
policy. Skipping that prompt never advances reviewed state. `review.pkgbuild`
and `--edit` / `--noedit` select invocation-local PKGBUILD / `.install` editor
behavior and do not act as upstream review acceptance.

A missing config file is normal. An existing file must contain
`schema_version = 1`; invalid TOML, unknown keys, type errors, invalid enum
values, and unsupported future schema versions stop the invocation. Moguet
does not create, rewrite, or migrate this file automatically, and it does not
use `/etc/moguet` as a system-wide configuration layer.

Source-build preferences are separate per-package files under:

```text
${XDG_CONFIG_HOME:-$HOME/.config}/moguet/source-build.d/<package-name>
```

They are not TOML config. `add-src`, `edit-src`, `list-src`, `del-src`,
`revert`, and every source-aware build or explicit `upgrade*` reader use this
one authority. Exact target-less `-Syu`, including its dry-run, deliberately
does not snapshot, enumerate, or read this directory and does not apply a
child- or PackageBase-named fallback preference. A missing store or entry
means no saved preference for strict readers; an invalid name, unsafe entry,
permission error, or I/O failure is a hard error. Read and list operations do
not create directories. Only an `add-src` or `edit-src` that first needs
storage creates the managed directories with mode `0700` and the entry with
mode `0600`. Package install, reinstall, and uninstall do not create,
migrate, or remove either XDG preferences or legacy data.

<!-- parity:xdg -->
## XDG config, source preferences, state, and cache

| Responsibility | XDG path | Fallback |
| --- | --- | --- |
| User configuration | `$XDG_CONFIG_HOME/moguet/` | `~/.config/moguet/` |
| Source-build preferences | `$XDG_CONFIG_HOME/moguet/source-build.d/` | `~/.config/moguet/source-build.d/` |
| Persistent runtime state, reviewed AUR revisions, and log | `$XDG_STATE_HOME/moguet/` | `~/.local/state/moguet/` |
| Reproducible cache | `$XDG_CACHE_HOME/moguet/` | `~/.cache/moguet/` |

The default log is `moguet.log` in the state directory. Cache contents are not
authoritative and may be regenerated; deleting cache must not delete config or
persistent state. Directories are created only when a command needs them.
Help and version output do not create XDG directories.

Accepted AUR revisions are stored below
`$XDG_STATE_HOME/moguet/reviewed-sources/aur/`, with the HOME fallback below
`~/.local/state/moguet/`. They are PackageBase state, not cache metadata, and
survive cache deletion or recloning. A read-only lookup does not create this
store.

For source-preference access, an unset or empty `XDG_CONFIG_HOME` uses the HOME
fallback. An explicit `XDG_CONFIG_HOME` must be absolute, already exist, and
pass the ownership, type, and permission checks; Moguet creates only the
managed `moguet/source-build.d` children when a write command first needs
them. With the HOME fallback, the same source-preference safety boundary may
create the required config hierarchy. Relative or otherwise unsafe XDG paths
fail closed. When Moguet is run as root, root's own XDG context is used;
Moguet does not infer a different user from `SUDO_USER` or write into that
user's home. This path rule does not make AUR source builds as root acceptable.

<!-- parity:localization -->
## Localization

English text is the source authority and built-in fallback. Japanese is a
formally supported translation. Moguet follows the process locale through
standard GNU gettext behavior (`LC_ALL`, `LC_MESSAGES`, `LANG`, and
`LANGUAGE`) and does not implement `--lang` or a language setting in TOML.

Use `LC_ALL=C` when an English reproduction is useful. If a catalog or entry
is unavailable, the complete English message remains visible. Command and
option tokens, package and repository identities, paths, TOML keys and enum
values, machine-readable fields, and output produced by external programs are
not translated.

English and Japanese man pages use the standard locale-specific layout, so
`man moguet` selects the Japanese page when appropriate and otherwise falls
back to English.

<!-- parity:compatibility -->
## Compatibility and migration

Moguet is pacman-first, not fully pacman-compatible in every source-build
route. Operations handled entirely by pacman pass through options that Moguet
does not consume. When Moguet takes responsibility for an AUR or source-build
route, it preserves only options with an explicitly defined equivalent and
fails before mutation for the rest.

Moguet v2.6.0 changes exact target-less `moguet -Syu` from repository-only to
the ordinary AUR-helper behavior: repository system update followed by a
normal installed-AUR update, without reading or applying saved source-build
preferences. Existing users who require the former repository-only behavior
must migrate to `moguet -Syu --repo`. Use `moguet upgrade`,
`moguet upgrade-aur`, or `moguet upgrade-all` when saved source-build
preferences must be checked and applied. The new combined route is sequential,
not atomic; a later AUR failure does not roll back a completed repository
transaction and is reported as partial completion with a non-zero status.

No manual migration is required for an AUR cache created before reviewed-source
state existed. The absence of a record is normal and causes one initial full
review for the first affected PackageBase. Moguet never invents a reviewed
revision from the legacy checkout HEAD, branch, remote ref, or build artifacts.
Invalid, corrupted, source-mismatched, future, or unsafe state is not treated
as missing and remains fail-closed according to the reviewed-source contract.

Moguet does not read `/etc/jpacker/jpacker.conf` as a normal config layer or
use `/etc/jpacker/package.build/` as a source-preference fallback. It does not
automatically copy, merge, rewrite, or delete `/etc/jpacker`. Root-owned legacy
data is not assigned to a user by guesswork. Back up the v1 state and follow
the [English migration guide](docs/migration/v1-to-v2.md) or [Japanese
migration guide](docs/migration/v1-to-v2.ja.md) to recreate understood entries
manually for each target user and to preserve rollback data.

The formal and only packaged v2 command is `moguet`; no `jpacker` binary alias
is supplied. Moguet and jpacker v1.16.0 have disjoint package files and may be
installed together for transition and rollback. Do not run mutating operations
from both helpers concurrently. Their preference stores are not shared. Moguet
ignores and preserves the legacy store; only an explicit per-user migration
changes the XDG authority.

<!-- parity:development -->
## Development

The canonical development repository is
[GitHub](https://github.com/seekerkrt/moguet), with a
[GitLab mirror](https://gitlab.com/seekerkrt/moguet). Questions, suspected
bugs, and feature ideas should start in
[GitHub Discussions](https://github.com/seekerkrt/moguet/discussions).
[GitHub Issues](https://github.com/seekerkrt/moguet/issues) track concrete work
managed by the maintainer; a reproducible bug with sufficient observation
details may be submitted directly through the
[Bug Issue Form](https://github.com/seekerkrt/moguet/issues/new?template=bug-report.yml).
Pull requests are managed on GitHub.

See
[CONTRIBUTING.md](https://github.com/seekerkrt/moguet/blob/develop/CONTRIBUTING.md)
before proposing a change. Do not post security-sensitive details in public
Discussions or Issues; follow
[SECURITY.md](https://github.com/seekerkrt/moguet/blob/develop/SECURITY.md) and
[report them privately](https://github.com/seekerkrt/moguet/security/advisories/new).

The active integration branch is `develop`; stable releases are on `main`.
See
[docs/DEVELOPMENT.md](https://github.com/seekerkrt/moguet/blob/develop/docs/DEVELOPMENT.md),
and
[docs/VERSIONING.md](https://github.com/seekerkrt/moguet/blob/develop/docs/VERSIONING.md).
Moguet v2.x will add AUR-helper
capabilities incrementally; advanced runtime-aware completion and the later
build-profile system are separate work.

<!-- parity:license -->
## License

Moguet releases and jpacker v1.15.0 through v1.16.0 are distributed under `GPL-3.0-or-later`.
jpacker v1.14.0 and earlier releases were distributed under the MIT License.
Those historical releases remain
available under their original license; their tags, releases, and granted
permissions are unchanged by the Moguet rename.

- GNU GPL version 3 full text: [LICENSE](https://github.com/seekerkrt/moguet/blob/develop/LICENSE)
- Version boundary and distribution policy: [docs/LICENSING.md](docs/LICENSING.md)
- Linked/compiled components and external programs: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
- Historical MIT text for v1.14.0 and earlier: [LICENSES/jpacker-MIT-legacy.txt](https://github.com/seekerkrt/moguet/blob/develop/LICENSES/jpacker-MIT-legacy.txt)

Moguet directly and dynamically links libalpm and libcurl and compiles the
system nlohmann-json and toml++ headers into its binary. pacman, pacman-conf,
makepkg, git, vercmp, and the other programs listed in the notices remain
separate programs invoked across a process boundary.
