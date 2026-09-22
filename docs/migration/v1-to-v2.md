# Migrating from jpacker v1 to Moguet v2

[日本語](v1-to-v2.ja.md)

<!-- parity:overview -->
## Overview

Moguet v2.0.0 is a breaking transition from jpacker v1.16.0. The execution
base remains pacman-first, but the public identity, user storage, configuration
format, and localized documentation change together.

This guide defines a non-destructive order:

```text
inspect and back up jpacker v1 data
-> install the validated Moguet v2 package alongside jpacker
-> migrate only understood settings by hand
-> verify command, man, completion, locale, and paths
-> optionally remove jpacker after rollback validation
```

Moguet does not use `/etc/jpacker/jpacker.conf` as a normal configuration
layer. It does not automatically copy, rewrite, merge, or delete
`/etc/jpacker`, and it does not guess which user should receive root-owned
data. Starting with v2.0.1, `/etc/jpacker/package.build/` is also legacy input
for manual migration only: Moguet neither reads nor writes it at runtime. The
v2.0.0 tag, Release, and release notes remain unchanged historical artifacts.

<!-- parity:preparation -->
## Before you begin

1. Finish or stop package operations. Do not migrate while pacman, makepkg, or
   another package helper is changing the system.
2. Confirm that the installed legacy package is jpacker v1.16.0, and record
   how it was installed.
3. Read the release-specific Moguet package instructions. The v2 package has
   no file conflict or metadata relationship with jpacker v1.16.0, so both may
   remain installed during validation. It supplies no `jpacker` command alias.
4. Keep a trusted jpacker v1.16.0 package or source archive available for
   rollback.
5. Plan the migration separately for every local user. A root-owned legacy
   directory does not identify a destination user.

For example, record the installed package before making changes:

```bash
pacman -Q jpacker
pacman -Qi jpacker
```

If those commands report no package, determine the actual install method
before using package removal commands.

<!-- parity:identity -->
## Identity changes

| Surface | jpacker v1.16.0 | Moguet v2.0.0 |
| --- | --- | --- |
| Project / brand | jpacker | Moguet |
| Reading | — | モグエット |
| CLI / binary | `jpacker` | `moguet` |
| Package | `jpacker` | `moguet` |
| XDG application name | legacy jpacker paths | `moguet` |
| Project environment prefix | legacy names | `MOGUET_*` |
| Runtime localization | English-only legacy surface | English authority, Japanese formal translation |

The formal project spelling is `Moguet`, and its Japanese reading is
`モグエット`. Command and option tokens are not translated. The formal v2
command is `moguet`; do not create a local `jpacker` symlink and assume that it
has packaging support.
The package intentionally declares no `provides`, `conflicts`, or `replaces`
relationship with `jpacker`: it neither implements the old command nor removes
the rollback package as an implicit upgrade.

The canonical source identity is now `seekerkrt/moguet` on GitHub and GitLab.
The old `seekerkrt/jpacker` URLs are redirect-only legacy entry points and must
not be reused for a different repository.

<!-- parity:backup -->
## Back up v1 data

Create a private backup before changing either package. At minimum, preserve:

- the installed jpacker version and package metadata;
- `/etc/jpacker/jpacker.conf`, if present;
- `/etc/jpacker/package.build/`, including ownership and modes, if present;
- any custom log location referenced by `LOGFILE`;
- any package/source files that you changed outside the normal package
  payload.

One possible archive workflow is:

```bash
backup_root="$HOME/moguet-migration-backup-$(date +%Y%m%d-%H%M%S)"
install -d -m 700 "$backup_root"
pacman -Q jpacker > "$backup_root/jpacker-package.txt"
pacman -Qi jpacker > "$backup_root/jpacker-package-info.txt"
sudo tar --acls --xattrs -C /etc -cpf - jpacker \
    > "$backup_root/etc-jpacker.tar"
tar -tf "$backup_root/etc-jpacker.tar"
```

If `/etc/jpacker` does not exist, skip only that archive step. A failed or
empty archive is not a backup; inspect the command status and listing. Store a
copy outside the machine if losing the settings would be costly.

Also preserve any pre-existing Moguet XDG directories before testing a new
package. Their standard locations are:

```text
${XDG_CONFIG_HOME:-$HOME/.config}/moguet
${XDG_STATE_HOME:-$HOME/.local/state}/moguet
${XDG_CACHE_HOME:-$HOME/.cache}/moguet
```

Do not merge those directories with `/etc/jpacker` during backup.

<!-- parity:remove-v1 -->
## Keep or explicitly remove jpacker v1.16.0

The validated Moguet and jpacker v1.16.0 payloads have no common file, so keep
jpacker installed while checking Moguet when practical. This provides the
shortest rollback: remove Moguet and continue with the unchanged `jpacker`
command. Their source-preference stores are separate, but do not run package-
mutating operations from the two helpers concurrently because both may invoke
the same system package tools.

After Moguet verification, you may explicitly remove jpacker with the same
package manager that installed it. For a normal pacman-managed installation,
the conservative form is:

```bash
sudo pacman -R jpacker
```

Do not add recursive dependency cleanup merely for the rename. Inspect
pacman's proposed transaction before confirming it. If the package was
installed by another method, use that method's documented uninstall path
instead of pretending pacman owns it.

Package removal must not be used as a cleanup command for `/etc/jpacker`.
Keep the backup and any preserved `.pacsave` or preference files until both
migration and rollback validation are complete.

<!-- parity:install-v2 -->
## Install Moguet v2

Install Moguet after the v2 package source, signature/checksum, dependencies,
file-conflict audit, and payload have been verified by the release-specific
instructions. jpacker does not need to be removed first.

The package identity is `moguet` and its only executable is `/usr/bin/moguet`;
there is no `/usr/bin/jpacker` alias. The package metadata has no `provides`,
`conflicts`, or `replaces` entry for jpacker. Coexistence is a transition and
rollback property, not a claim that Moguet provides the jpacker interface.
Moguet v2.0.0 does not include AUR publication, so this guide intentionally
does not invent an AUR URL or a `pacman -S` repository command. Do not stage a
development `make install` over the old package as a substitute for the
validated transition.

Installing the package must not create `/etc/moguet`, a user XDG config file,
the source-preference directory, or user XDG state/cache directories. A
command creates only the user directories that its operation actually needs,
under that executing user's own XDG context.

<!-- parity:configuration -->
## Migrate configuration manually

Moguet uses this optional user-owned file:

```text
$XDG_CONFIG_HOME/moguet/config.toml
fallback: ~/.config/moguet/config.toml
```

Create it only for a specific user who wants non-default values. The minimal
schema is:

```toml
schema_version = 1

[review]
pkgbuild = "prompt"
diff = "prompt"

[build]
mode = "normal"
```

Map only understood jpacker v1 keys:

| jpacker v1 setting | Moguet v2 action |
| --- | --- |
| `NOEDIT=true` | Set `review.pkgbuild = "skip"` |
| `NOEDIT=false` | Omit the key or use `review.pkgbuild = "prompt"` |
| `NODIFF=true` | Set `review.diff = "skip"` |
| `NODIFF=false` | Omit the key or use `review.diff = "prompt"` |
| `EDITOR=...` | Do not copy to TOML; set `VISUAL`, then `EDITOR`, in the user's environment |
| `LOGFILE=...` | No v2.0.0 config key; use the fixed XDG state log |
| `RMDEPS=true` | Do not migrate; v2 has no persistent `RMDEPS` config key. Dependency cleanup is an explicit per-invocation `--rmdeps` request, currently supported only for remote AUR builds. |

The editor resolution order is `VISUAL -> EDITOR -> nano`. The default log is:

```text
$XDG_STATE_HOME/moguet/moguet.log
fallback: ~/.local/state/moguet/moguet.log
```

Moguet reads configuration as strict, read-only input. An existing file needs
`schema_version = 1`; unknown keys, invalid types or enum values, and future
schema versions fail before external mutation. Moguet does not rewrite the
file or silently fall back from a broken file.

<!-- parity:legacy-data -->
## Handle legacy preferences and data

Moguet's canonical source-build preference entry is:

```text
${XDG_CONFIG_HOME:-$HOME/.config}/moguet/source-build.d/<package-name>
```

An unset or empty `XDG_CONFIG_HOME` uses the `$HOME/.config` fallback. An
explicit `XDG_CONFIG_HOME` must be an absolute, safe, existing directory;
relative or otherwise unsafe values fail closed. The fallback path may be
created safely when the first write needs it. Moguet uses the executing user's
own XDG context, including when the CLI runs as root, and never infers another
user from `SUDO_USER`.

All source-preference commands and the build/upgrade readers use only this
authority. Reads, `moguet list-src`, and a missing `moguet del-src` or
`moguet revert` do not create a missing source-preference store. The first
`moguet add-src <pkg> [V=K]` or `moguet edit-src <pkg>` that needs storage
is the only source-preference operation that creates the managed
`moguet/source-build.d` hierarchy, with mode `0700`; entries use mode `0600`.
Filesystem preference operations do not use `sudo`. `moguet revert` may still
use `sudo` for its separate pacman transaction.

Treat `/etc/jpacker/package.build/` as legacy input for manual migration only.
Moguet does not read, fall back to, merge with, rewrite, or delete it at
runtime. It also does not automatically copy a legacy entry into XDG storage.
The package installer, reinstaller, and uninstaller preserve both legacy and
canonical entries and do not create or remove user XDG directories.

Migrate one understood package for one explicitly selected user at a time:

1. Keep the verified legacy backup and inspect the legacy entry without
   modifying it.
2. As the target user, confirm that the canonical destination entry is absent.
   A symlink or another unexpected object is not an absent entry and must be
   investigated rather than overwritten.
3. If you set `XDG_CONFIG_HOME` explicitly, create and validate that base
   directory separately before running Moguet. Do not point it at a relative
   or shared directory.
4. Re-enter only understood assignments with one of these interfaces, without
   `sudo`:

   ```bash
   moguet add-src <package-name> [V=K ...]
   moguet edit-src <package-name>
   ```

5. Verify the complete resulting snapshot before considering that package
   migrated:

   ```bash
   moguet list-src
   ```

Do not automate a bulk loop, overwrite a canonical entry, or merge values that
you have not reviewed. A package name is validated before directory creation
or editor execution. Only a missing store or entry means “absent”; an invalid
entry name, symlink, non-regular file, ownership/mode violation, permission or
I/O error, or detected race is a hard error. `moguet list-src` validates the
whole snapshot before printing anything, so a bad entry cannot produce a
trusted partial listing.

Moguet does not automatically:

- read `/etc/jpacker/jpacker.conf` as a config layer;
- copy `/etc/jpacker` into one or more users' homes;
- read, merge, delete, or rewrite legacy source-preference files;
- create or read `/etc/moguet`;
- migrate `LOGFILE`, `RMDEPS`, arbitrary editor commands, credentials, shell
  fragments, or unknown uppercase keys;
- infer a destination user from root ownership, `sudo`, or `SUDO_USER`.

Config, source preferences, state, and cache have separate v2 responsibilities:

```text
config:             $XDG_CONFIG_HOME/moguet/config.toml
source preferences: $XDG_CONFIG_HOME/moguet/source-build.d/
state:              $XDG_STATE_HOME/moguet/  (fallback ~/.local/state/moguet/)
cache:              $XDG_CACHE_HOME/moguet/  (fallback ~/.cache/moguet/)

config/source-preference fallback: ~/.config/moguet/
```

Cache is reproducible and is not a backup location. Uninstalling Moguet must
not be treated as permission to remove user config, state, or cache.

<!-- parity:verification -->
## Verify the migration

Run Moguet as the target normal user, not through `sudo`:

```bash
command -v moguet
moguet --version
LC_ALL=C moguet --help
```

Then verify documentation and completion files supplied by the package:

```bash
man -w moguet
LC_ALL=C man moguet
LANG=ja_JP.UTF-8 man moguet
```

Expected standard paths are:

```text
/usr/share/man/man1/moguet.1
/usr/share/man/ja/man1/moguet.1
/usr/share/bash-completion/completions/moguet
/usr/share/zsh/site-functions/_moguet
/usr/share/fish/vendor_completions.d/moguet.fish
/usr/share/locale/ja/LC_MESSAGES/moguet.mo
/usr/share/licenses/moguet/LICENSE
/usr/share/licenses/moguet/jpacker-MIT-legacy.txt
/usr/share/licenses/moguet/curl.txt
/usr/share/licenses/moguet/nlohmann-json-MIT.txt
/usr/share/licenses/moguet/tomlplusplus-MIT.txt
/usr/share/licenses/moguet/bjoern-hoehrmann-utf8-MIT.txt
/usr/share/doc/moguet/README.md
/usr/share/doc/moguet/README.ja.md
/usr/share/doc/moguet/THIRD_PARTY_NOTICES.md
/usr/share/doc/moguet/docs/LICENSING.md
/usr/share/doc/moguet/docs/migration/v1-to-v2.md
/usr/share/doc/moguet/docs/migration/v1-to-v2.ja.md
```

Start a fresh shell, or reload only the completion mechanism documented by
your shell. If the system uses a man-page cache, use its normal package hook or
administrator procedure to refresh it. Confirm that completion proposes
`moguet` commands and the final options such as `--edit`, `--diff`, and
`--build-mode=normal|rebuild|clean`, not a legacy `jpacker` command.

Finally, inspect the resolved XDG paths as the intended user. A help/version
check does not create XDG consumer directories. `moguet list-src` and source-
preference reads performed by read, build, or upgrade paths do not create a
missing source-preference store. They are otherwise normal commands, however:
under the existing state-logging contract, `moguet list-src` and other normal
commands may create the state directory and log. If you migrated preferences,
confirm that the managed directories are mode `0700`, entries are mode `0600`,
the legacy entries remain unchanged, and the complete `moguet list-src` output
matches the intended packages. Test a package operation only after reviewing
its external commands and effects.

<!-- parity:rollback -->
## Roll back to jpacker v1.16.0

Rollback is an explicit package transition, not an automatic transaction
rollback.

1. Stop Moguet and finish any active package operation.
2. Back up the user's Moguet config, source preferences, and state. Keep cache
   only if it is useful for diagnostics.
3. Remove the Moguet package using the package manager that installed it.
   Do not delete user XDG directories as part of package removal.
4. If jpacker was kept installed, verify that its package files and command are
   unchanged. If it was removed, reinstall the trusted jpacker v1.16.0 package
   or source archive recorded before migration.
5. Restore `/etc/jpacker` only from the verified backup and only after checking
   the current destination. Do not overwrite a newer file blindly.
6. Verify `jpacker --version`, the v1 man/completion surface, and a read-only
   operation before performing a package transaction.

Moguet's XDG data is not interpreted by jpacker v1 and may be kept for a later
retry. It is not synchronized back into `/etc/jpacker/package.build/`.
A completed pacman transaction is not undone by switching helpers; compare
actual package database state rather than assuming the helper rollback changed
installed packages.

<!-- parity:maintenance -->
## v1 maintenance and repository remotes

jpacker v1.16.0 remains under the `jpacker` identity in its immutable tag and
Release. It is not relabeled as Moguet and does not receive the v2 XDG/config
format through this migration guide. There is no permanent v1 maintenance
branch. If a critical fix is required, its branch starts from the `v1.16.0`
tag instead of treating `develop` as a v1 source.

The canonical repository URLs are:

- GitHub: `https://github.com/seekerkrt/moguet`
- GitLab mirror: `https://gitlab.com/seekerkrt/moguet`

The former `https://github.com/seekerkrt/jpacker` and
`https://gitlab.com/seekerkrt/jpacker` URLs are retained only as redirects.
Do not create a new project at either old slug. Verify that an old bookmark or
v1 Release link reaches the Moguet repository before relying on the redirect.

For an existing clone, inspect its remotes and update each configured remote:

```bash
git remote -v
git remote set-url origin https://github.com/seekerkrt/moguet.git
git remote set-url gitlab https://gitlab.com/seekerkrt/moguet.git
git remote get-url origin
git remote get-url gitlab
```

If the clone uses SSH, use `git@github.com:seekerkrt/moguet.git` and
`git@gitlab.com:seekerkrt/moguet.git` instead. Skip the `gitlab` commands when
that remote is not configured. Changing a remote URL does not migrate package
state or `/etc/jpacker` data.

For the current source contracts, see [README.md](../../README.md),
[README.ja.md](../../README.ja.md), and
[compatibility.md](https://github.com/seekerkrt/moguet/blob/develop/docs/compatibility.md).
