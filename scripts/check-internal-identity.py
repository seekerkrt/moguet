#!/usr/bin/env python3

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import tarfile


REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
CURRENT_COMMAND = "moguet"
LEGACY_NAME = "j" + "packer"
LEGACY_TITLE = LEGACY_NAME.capitalize()
LEGACY_UPPER = LEGACY_NAME.upper()
FORMER_CANDIDATE = "pac" + "tune"
REJECTED_ROMANIZATION = "MUG" + "UET"
REJECTED_JAPANESE_NAME = "\u30df\u30e5\u30b2"
OBSOLETE_SOURCE_PREFERENCE_OVERRIDE = "MOGUET_TEST_" + "PACKAGE_BUILD_DIR"
FORBIDDEN_RUNTIME_SYSTEM_PATHS = (
    "/etc/" + LEGACY_NAME,
    "/etc/moguet",
)
RUNTIME_AUTHORITY_PATHS = {"Makefile", "PKGBUILD"}
CURRENT_SOURCE_ARCHIVE_OVERRIDE = "MOGUET_TEST_CURRENT_SOURCE_ARCHIVE"


@dataclass(frozen=True)
class Finding:
    check: str
    path: str
    line_number: int
    line: str


@dataclass(frozen=True)
class LegacyAllowance:
    category: str
    pattern: re.Pattern[str]


def fail(message: str) -> None:
    print(f"internal-identity-audit: {message}", file=sys.stderr)
    raise SystemExit(1)


def source_archive_paths(archive_value: str) -> list[str]:
    archive_path = Path(archive_value)
    try:
        archive_mode = archive_path.lstat().st_mode
    except OSError as error:
        fail(f"unable to inspect current source archive {archive_path}: {error}")
    if archive_path.is_symlink() or not stat.S_ISREG(archive_mode):
        fail(f"current source archive is not a regular non-symlink: {archive_path}")

    try:
        with tarfile.open(archive_path, mode="r:*") as source_archive:
            members = source_archive.getmembers()
    except (OSError, tarfile.TarError) as error:
        fail(f"unable to read current source archive {archive_path}: {error}")

    paths: list[str] = []
    for member in members:
        if not member.isfile():
            continue
        member_path = PurePosixPath(member.name)
        normalized_parts = tuple(
            part for part in member_path.parts if part not in ("", ".")
        )
        if (
            member_path.is_absolute()
            or not normalized_parts
            or ".." in normalized_parts
        ):
            fail(f"unsafe current source archive path: {member.name}")
        path = PurePosixPath(*normalized_parts).as_posix()
        if (REPOSITORY_ROOT / path).is_file():
            paths.append(path)
    return sorted(set(paths))


def repository_paths() -> list[str]:
    source_archive_override = os.environ.get(CURRENT_SOURCE_ARCHIVE_OVERRIDE, "")
    if source_archive_override:
        return source_archive_paths(source_archive_override)

    result = subprocess.run(
        [
            "git",
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
        ],
        cwd=REPOSITORY_ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        fail(
            "unable to enumerate repository files: "
            + result.stderr.decode("utf-8", errors="replace").strip()
        )

    paths: list[str] = []
    for raw_path in result.stdout.split(b"\0"):
        if not raw_path:
            continue
        path = raw_path.decode("utf-8")
        absolute_path = REPOSITORY_ROOT / path
        if absolute_path.is_file():
            paths.append(path)
    return sorted(set(paths))


def read_text(path: str) -> str | None:
    data = (REPOSITORY_ROOT / path).read_bytes()
    if b"\0" in data:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def is_active_identity_path(path: str) -> bool:
    return (
        path in {".gitignore", "Makefile"}
        or path.startswith("source/")
        or path.startswith("tests/")
        or path.startswith("scripts/")
    )


MIGRATION_DOCUMENTS = {
    "docs/migration/v1-to-v2.md",
    "docs/migration/v1-to-v2.ja.md",
}
PUBLIC_MAN_DOCUMENTS = {
    "man/moguet.1",
    "man/moguet.1.in",
    "man/ja/moguet.1",
    "man/ja/moguet.1.in",
}
HISTORICAL_DOCUMENTS = {
    "docs/audit/v1.8.0-claude-code.md",
    "docs/audit/v1.8.0-codex.md",
}
HISTORICAL_LEGACY_PATHS = {
    f"LICENSES/{LEGACY_NAME}-MIT-legacy.txt",
}


def allowances(category: str, *patterns: str) -> tuple[LegacyAllowance, ...]:
    return tuple(
        LegacyAllowance(category, re.compile(pattern)) for pattern in patterns
    )


legacy = re.escape(LEGACY_NAME)
identity_start = r"(?<![A-Za-z0-9_.-])"
identity_end = r"(?=$|/|[\"'`\s,;:#)=}\]]|\.(?=[\"'`\s]|$))"
historical_license_filename = (
    rf"{identity_start}{legacy}-MIT-legacy\.txt(?![A-Za-z0-9_.-])"
)
historical_project_version = (
    rf"{identity_start}{legacy} v1\.(?:14\.0|15\.0|16\.0)"
    rf"(?![0-9]|\.[0-9])"
)
legacy_repository_redirect = (
    rf"seekerkrt/{legacy}(?:\.git)?{identity_end}"
)
legacy_etc_path = (
    rf"/etc/{legacy}(?:/[A-Za-z0-9@._+-]+)*(?![A-Za-z0-9_.-])"
)
config_filename = rf"{identity_start}{legacy}\.conf{identity_end}"
log_filename = rf"{identity_start}{legacy}\.log{identity_end}"
xdg_cache_component = rf"\$XDG_CACHE_HOME/{legacy}{identity_end}"
case_cache_component = rf"\$case_dir/xdg-cache/{legacy}{identity_end}"
legacy_cache_phrase = (
    rf"legacy {legacy}(?: cache(?: root| symlink)?| path component|\.log file|"
    rf" package directories){identity_end}"
)
legacy_source_archive_filename = (
    rf"{identity_start}{legacy}-v1\.16\.0-source\.tar"
    rf"(?![A-Za-z0-9_.-])"
)


FINAL_REPOSITORY_TOKENS: dict[str, tuple[str, ...]] = {
    "README.md": (
        "https://github.com/seekerkrt/moguet",
        "https://gitlab.com/seekerkrt/moguet",
    ),
    "README.ja.md": (
        "https://github.com/seekerkrt/moguet",
        "https://gitlab.com/seekerkrt/moguet",
    ),
    "RELEASE_NOTES.md": (
        "https://github.com/seekerkrt/moguet",
        "https://gitlab.com/seekerkrt/moguet",
    ),
    "CONTRIBUTING.md": ("https://github.com/seekerkrt/moguet/issues",),
    "PKGBUILD": ("https://github.com/seekerkrt/moguet",),
    "THIRD_PARTY_NOTICES.md": (
        "https://github.com/seekerkrt/moguet/blob/develop/",
    ),
    "docs/development.md": (
        "https://github.com/seekerkrt/moguet",
        "https://gitlab.com/seekerkrt/moguet",
    ),
    "docs/LICENSING.md": (
        "https://github.com/seekerkrt/moguet/blob/develop/",
    ),
    "docs/migration/v1-to-v2.md": (
        "https://github.com/seekerkrt/moguet",
        "https://gitlab.com/seekerkrt/moguet",
    ),
    "docs/migration/v1-to-v2.ja.md": (
        "https://github.com/seekerkrt/moguet",
        "https://gitlab.com/seekerkrt/moguet",
    ),
    "tests/fixtures/current-package/contract.env": (
        "COMMAND_NAME=moguet",
        "https://github.com/seekerkrt/moguet",
    ),
    ".github/workflows/mirror-gitlab.yml": (
        "git@gitlab.com:seekerkrt/moguet.git",
    ),
    ".github/workflows/mirror-gitlab-release.yml": (
        "seekerkrt%2Fmoguet",
    ),
    ".gitlab/mirror-github-release.sh": ("seekerkrt/moguet",),
}

FINAL_REPOSITORY_TOKEN_COUNTS = {
    (".github/workflows/mirror-gitlab.yml", "seekerkrt/moguet"): 1,
    (".github/workflows/mirror-gitlab-release.yml", "seekerkrt%2Fmoguet"): 2,
    (".gitlab/mirror-github-release.sh", "seekerkrt/moguet"): 1,
}


ACTIVE_LEGACY_ALLOWANCES: dict[str, tuple[LegacyAllowance, ...]] = {
    "CMakeLists.txt": allowances(
        "historical-license-file",
        historical_license_filename,
    ),
    "AGENTS.md": allowances(
        "legacy-storage-path",
        legacy_etc_path,
    ),
    "CLAUDE.md": allowances(
        "legacy-storage-path",
        legacy_etc_path,
    ),
    "README.md": (
        allowances(
            "historical-project-version",
            historical_project_version,
        )
        + allowances(
            "historical-license-file",
            historical_license_filename,
        )
        + allowances(
            "legacy-storage-path",
            legacy_etc_path,
        )
        + allowances(
            "negative-package-assertion",
            rf"/usr/bin/{legacy}{identity_end}",
            rf"(?:no |provide a ){identity_start}`?{legacy}`? (?:command|binary) alias",
            rf"relationship with `{legacy}`",
        )
    ),
    "README.ja.md": (
        allowances(
            "historical-project-version",
            historical_project_version,
        )
        + allowances(
            "historical-license-file",
            historical_license_filename,
        )
        + allowances(
            "legacy-storage-path",
            legacy_etc_path,
        )
        + allowances(
            "negative-package-assertion",
            rf"/usr/bin/{legacy}{identity_end}",
            rf"`{legacy}` (?:command|binary) alias",
            rf"`{legacy}`への`provides`",
        )
    ),
    "RELEASE_NOTES.md": (
        allowances(
            "historical-project-version",
            historical_project_version,
        )
        + allowances(
            "legacy-storage-path",
            legacy_etc_path,
        )
        + allowances(
            "negative-package-assertion",
            rf"no `{legacy}` command alias",
            rf"`{legacy}` command alias",
            rf"from `{legacy}` to `moguet`",
            rf"名は`{legacy}`から`moguet`",
        )
        + allowances(
            "legacy-repository-redirect",
            legacy_repository_redirect,
        )
    ),
    "docs/coding-conventions.md": allowances(
        "legacy-storage-path",
        legacy_etc_path,
    ),
    "docs/decisions.md": (
        allowances(
            "historical-project-version",
            historical_project_version,
        )
        + allowances(
            "legacy-cache-preservation",
            rf"legacy {legacy} cache(?![A-Za-z0-9_.-])",
        )
        + allowances(
            "historical-license-version",
            rf"{identity_start}{legacy} releases through v1\.14\.0",
        )
        + allowances(
            "legacy-storage-negative-contract",
            rf"`/etc/{legacy}`と`/etc/moguet`はruntime",
            rf"neither creates nor reads `/etc/{legacy}` or `/etc/moguet` at runtime",
        )
    ),
    "docs/compatibility.md": allowances(
        "legacy-storage-negative-contract",
        rf"`/etc/{legacy}`と`/etc/moguet`をruntime",
    ),
    "docs/versioning.md": (
        allowances(
            "historical-project-version",
            historical_project_version,
        )
        + allowances(
            "legacy-storage-path",
            legacy_etc_path,
        )
    ),
    ".gitignore": allowances(
        "production-artifact-cleanup",
        rf"^\s*/{legacy}\s*$",
    ),
    ".dockerignore": allowances(
        "production-artifact-cleanup",
        rf"^\s*/{legacy}\s*$",
    ),
    "Makefile": (
        allowances(
            "historical-license-file",
            historical_license_filename,
        )
        + allowances(
            "historical-package-fixture",
            legacy_source_archive_filename,
        )
    ),
    "PKGBUILD": allowances(
        "historical-package-transition",
        rf"{identity_start}{legacy} v1\.16\.0(?![0-9.])",
    )
    + allowances(
        "negative-alias-assertion",
        rf"no {legacy} command alias{identity_end}",
    ),
    "THIRD_PARTY_NOTICES.md": allowances(
        "historical-package-version",
        rf"historical {legacy} versions{identity_end}",
    ),
    "docs/LICENSING.md": allowances(
        "historical-project-identity",
        rf"前身である{legacy}(?=の)",
        historical_project_version,
    )
    + allowances(
        "historical-license-file",
        historical_license_filename,
    ),
    "scripts/check-packaging-metadata.sh": allowances(
        "historical-license-file",
        historical_license_filename,
    )
    + allowances(
        "negative-alias-assertion",
        rf"/usr/bin/{legacy}{identity_end}",
        rf"{identity_start}{legacy} binary alias{identity_end}",
    ),
    "scripts/check-license-compliance.sh": allowances(
        "historical-license-file",
        historical_license_filename,
    )
    + allowances(
        "historical-version-boundary",
        historical_project_version,
        rf"v1\.15\.0[^\n]*{identity_start}{legacy}{identity_end}",
        rf"v1\.15\.0以降の{legacy}(?=は)",
        rf"{identity_start}{legacy} releases",
        rf"{identity_start}{legacy} tags{identity_end}",
    )
    + allowances(
        "negative-identity-pattern",
        rf"current\( \({legacy}\|Moguet\)\)\?",
    ),
    "containers/arch-validation/Dockerfile": allowances(
        "historical-package-fixture",
        legacy_source_archive_filename,
    ),
    "containers/arch-validation/run-tests.sh": allowances(
        "historical-package-fixture",
        legacy_source_archive_filename,
    ),
    "source/app_config.cpp": allowances(
        "storage-path",
        rf"/etc/{legacy}/{legacy}\.conf{identity_end}",
    ),
    "source/app_config.hpp": allowances(
        "storage-path",
        rf"legacy {legacy}\.conf{identity_end}",
    ),
    "source/moguet.cpp": allowances(
        "storage-path",
        rf"legacy {legacy}\.conf{identity_end}",
    ),
    "tests/test-help-man-completion.sh": allowances(
        "deferred-artifact-contract-test",
        rf"man/{legacy}\.8(?:\.in)?{identity_end}",
        rf"completions/{legacy}_completion\.bash{identity_end}",
        rf"{identity_start}_{legacy}_global_options{identity_end}",
        rf"completions/_{legacy}{identity_end}",
        rf"{identity_start}{legacy}_global_options{identity_end}",
        rf"completions/{legacy}\.fish{identity_end}",
    ),
    "tests/test-upgrade-all-completion.sh": allowances(
        "deferred-artifact-contract-test",
        rf"completions/{legacy}_completion\.bash{identity_end}",
        rf"{identity_start}_{legacy}{identity_end}",
        rf"run_completion\s+{legacy}(?=\s)",
    ),
    "tests/test-install-layout.sh": allowances(
        "negative-package-assertion",
        rf"/usr/bin/{legacy}{identity_end}",
    )
    + allowances(
        "historical-license-file",
        historical_license_filename,
    )
    + allowances(
        "legacy-storage-fixture",
        legacy_etc_path,
    ),
    "tests/test-package-transition.sh": allowances(
        "historical-package-fixture",
        rf"{identity_start}{legacy}-v1\.16\.0-(?:source(?:\.tar)?|stage|"
        rf"payload(?:\.tar|\.sha256)?|files\.txt|removable-files\.txt|"
        rf"makepkg|packages|archive-root|directories\.txt)"
        rf"(?![A-Za-z0-9_.-])",
        rf"{identity_start}{legacy}-1\.16\.0-1-x86_64\.pkg\.tar\.zst"
        rf"(?![A-Za-z0-9_.-])",
        rf"{identity_start}{legacy} v1\.16\.0(?![0-9.])",
        rf"{identity_start}{legacy} package transition fixture{identity_end}",
        rf"pkgname {legacy}{identity_end}",
        rf"usr/bin/{legacy}{identity_end}",
        rf"rollback {legacy} version{identity_end}",
    )
    + allowances(
        "historical-package-source",
        rf"git\+https://github\.com/seekerkrt/{legacy}\.git{identity_end}",
    )
    + allowances(
        "historical-license-file",
        historical_license_filename,
    )
    + allowances(
        "negative-package-assertion",
        rf"/(?:usr/bin/{legacy}|"
        rf"usr/share/bash-completion/completions/{legacy}|"
        rf"usr/share/zsh/site-functions/_{legacy}|"
        rf"usr/share/fish/vendor_completions\.d/{legacy}\.fish|"
        rf"usr/share/man/man8/{legacy}\.8(?:\.gz)?){identity_end}",
        rf"config/{legacy}\.conf{identity_end}",
    )
    + allowances(
        "legacy-storage-fixture",
        legacy_etc_path,
        rf"etc/{legacy}/{legacy}\.conf{identity_end}",
        rf"{identity_start}{legacy}\.conf(?:\.pacsave)?{identity_end}",
    ),
    "tests/fixtures/current-package/install-payload.txt": allowances(
        "historical-license-file",
        historical_license_filename,
    ),
    "tests/trusted_cache_test.cpp": (
        allowances(
            "legacy-cache-preservation-fixture",
            rf'cache_home / "{legacy}"',
            rf'legacy_root / "{legacy}\.log"',
            rf'create_symlink\("{legacy}\.log"',
            rf'Legacy {legacy} cache tree changed\.',
        )
        + allowances(
            "new-cache-ordinary-legacy-name-fixture",
            rf'root\.path\(\) / "{legacy}\.log"',
        )
    ),
    "tests/test-commands-sync.sh": allowances(
        "storage-fixture",
        config_filename,
        log_filename,
        xdg_cache_component,
    ),
    "tests/test-pkgbuild-export.sh": allowances(
        "storage-fixture",
        xdg_cache_component,
        legacy_cache_phrase,
    ),
    "tests/test-needed-contract.sh": allowances(
        "storage-fixture",
        xdg_cache_component,
        legacy_cache_phrase,
    ),
    "tests/test-aur-rpc-validation.sh": allowances(
        "storage-fixture",
        xdg_cache_component,
    ),
    "tests/test-source-build.sh": allowances(
        "storage-fixture",
        config_filename,
        case_cache_component,
    ),
    "tests/test-source-selection.sh": allowances(
        "storage-fixture",
        xdg_cache_component,
        legacy_cache_phrase,
    ),
    "tests/test-app-config.sh": allowances(
        "storage-fixture",
        config_filename,
        log_filename,
        xdg_cache_component,
    ),
    "tests/test-upgrade-all-command.sh": allowances(
        "storage-fixture",
        config_filename,
        xdg_cache_component,
    ),
    "tests/test-cli-parser.sh": allowances(
        "storage-fixture",
        xdg_cache_component,
    ),
    "tests/test-build-cache-symlink.sh": allowances(
        "storage-fixture",
        xdg_cache_component,
        rf"\$ancestor_target/{legacy}(?=/)",
        rf"{identity_start}{legacy}-escape{identity_end}",
        log_filename,
        legacy_cache_phrase,
    ),
    "tests/test-commands-source-maintenance.sh": allowances(
        "storage-fixture",
        config_filename,
        log_filename,
        case_cache_component,
    ),
    "tests/test-aur-update-command.sh": allowances(
        "storage-fixture",
        config_filename,
        xdg_cache_component,
        legacy_cache_phrase,
    ),
    "tests/stubs/source-maintenance/sudo": allowances(
        "storage-fixture",
        xdg_cache_component,
    ),
    "tests/stubs/commands-sync/sudo": allowances(
        "storage-fixture",
        xdg_cache_component,
    ),
    "tests/test-runtime-identity.sh": (
        allowances(
            "storage-fixture",
            config_filename,
            log_filename,
            rf"\$root_case_dir/xdg-cache/{legacy}(?=/|[\"']|\s|$)",
            rf"\$startup_case_dir/xdg-cache/{legacy}(?=/|[\"']|\s|$)",
            rf"\$fallback_case_dir/xdg-cache/{legacy}(?=/|[\"']|\s|$)",
        )
        + allowances(
            "negative-runtime-assertion",
            rf"grep -Fi -- '{legacy}'",
            rf"unintended {legacy} project identity",
            rf"assert_not_contains \"{legacy}\"",
            rf"assert_not_contains \"Started {legacy}\"",
        )
    ),
    "tests/test-localization.sh": allowances(
        "negative-runtime-assertion",
        rf"assert_not_contains 'legacy {legacy}\.conf'{identity_end}",
    ),
}


def classify_legacy_path(path: str) -> str | None:
    if path in HISTORICAL_LEGACY_PATHS:
        return "historical-license-file"
    return None


def classify_legacy_reference(
    path: str, line: str, match_start: int, match_end: int
) -> str | None:
    if path in HISTORICAL_DOCUMENTS:
        return "historical-audit-record"
    if path in MIGRATION_DOCUMENTS:
        return "migration-documentation"
    if path in PUBLIC_MAN_DOCUMENTS:
        return "public-man-migration-context"
    for allowance in ACTIVE_LEGACY_ALLOWANCES.get(path, ()):
        for context_match in allowance.pattern.finditer(line):
            if (
                context_match.start() <= match_start
                and match_end <= context_match.end()
            ):
                return allowance.category

    return None


def finding_for_line(check: str, path: str, line_number: int, line: str) -> Finding:
    return Finding(check, path, line_number, line.strip())


def legacy_categories_for_line(path: str, line: str) -> list[str | None]:
    pattern = re.compile(re.escape(LEGACY_NAME), re.IGNORECASE)
    return [
        classify_legacy_reference(path, line, match.start(), match.end())
        for match in pattern.finditer(line)
    ]


NON_HOOK_UPPERCASE_TOKENS = {
    "SIGNAL_TEST_RETRY_INTERVAL_MS",
    "SIGNAL_TEST_TIMEOUT",
}
TEST_INFRASTRUCTURE_TOKEN = re.compile(
    r"_TEST_(?:"
    r"TARGETS?|SRCS|SUPPORT_SRCS|CPPFLAGS|LDLIBS|OBJECT_DIR|OBJECTS|"
    r"LINK_OBJECTS|DEPS|COMPILE_SIGNATURE|LINK_SIGNATURE"
    r")$"
)


def extract_test_hook_tokens(text: str) -> set[str]:
    normalized_text = re.sub(r"-D(?=[A-Z])", "", text)
    uppercase_tokens = re.findall(r"\b[A-Z][A-Z0-9_]+\b", normalized_text)
    tokens: set[str] = set()
    for token in uppercase_tokens:
        if "_ENABLE_" not in token and "_TEST_" not in token:
            continue
        if token in NON_HOOK_UPPERCASE_TOKENS:
            continue
        if TEST_INFRASTRUCTURE_TOKEN.search(token):
            continue
        tokens.add(token)
    return tokens


def check_classifier_contract() -> None:
    allowed_categories = legacy_categories_for_line(
        "source/moguet.cpp", f'legacy {LEGACY_NAME}.conf: LOGFILE=...'
    )
    if allowed_categories != ["storage-path"]:
        fail("internal legacy-storage classifier self-test failed")

    rejected_external_url_categories = legacy_categories_for_line(
        "PKGBUILD", f'https://github.com/seekerkrt/{LEGACY_NAME}.git#tag=v2.0.0'
    )
    if rejected_external_url_categories != [None]:
        fail("internal legacy repository URL classifier is too broad")

    historical_document_categories = legacy_categories_for_line(
        "README.md", f"successor to {LEGACY_NAME} v1.16.0"
    )
    if historical_document_categories != ["historical-project-version"]:
        fail("internal historical-document classifier self-test failed")

    active_document_categories = legacy_categories_for_line(
        "README.md", f"{LEGACY_NAME} is the current project"
    )
    if active_document_categories != [None]:
        fail("internal current-document classifier is too broad")

    redirect_categories = legacy_categories_for_line(
        "RELEASE_NOTES.md", f"seekerkrt/{LEGACY_NAME} slugs remain redirects"
    )
    if redirect_categories != ["legacy-repository-redirect"]:
        fail("internal legacy redirect classifier self-test failed")

    historical_license_categories = legacy_categories_for_line(
        "docs/LICENSING.md", f"LICENSES/{LEGACY_NAME}-MIT-legacy.txt"
    )
    if historical_license_categories != ["historical-license-file"]:
        fail("internal historical-license classifier self-test failed")

    storage_fixture_categories = legacy_categories_for_line(
        "tests/test-install-layout.sh", f'legacy_config=/etc/{LEGACY_NAME}/config'
    )
    if storage_fixture_categories != ["legacy-storage-fixture"]:
        fail("internal package storage-fixture classifier self-test failed")

    negative_storage_contract_categories = legacy_categories_for_line(
        "docs/decisions.md",
        f"Moguet neither creates nor reads `/etc/{LEGACY_NAME}` or "
        "`/etc/moguet` at runtime",
    )
    if negative_storage_contract_categories != [
        "legacy-storage-negative-contract"
    ]:
        fail("internal negative storage-contract classifier self-test failed")

    active_storage_claim_categories = legacy_categories_for_line(
        "docs/decisions.md",
        f"Moguet reads `/etc/{LEGACY_NAME}` as its active store",
    )
    if active_storage_claim_categories != [None]:
        fail("internal negative storage-contract classifier is too broad")

    negative_alias_categories = legacy_categories_for_line(
        "scripts/check-packaging-metadata.sh",
        f"package must not provide a {LEGACY_NAME} binary alias.",
    )
    if negative_alias_categories != ["negative-alias-assertion"]:
        fail("internal negative-alias classifier self-test failed")

    historical_package_source_categories = legacy_categories_for_line(
        "tests/test-package-transition.sh",
        f"git+https://github.com/seekerkrt/{LEGACY_NAME}.git",
    )
    if historical_package_source_categories != ["historical-package-source"]:
        fail("internal historical-package-source classifier self-test failed")

    rejected_categories = legacy_categories_for_line(
        "source/moguet.cpp", f'char program[] = "{LEGACY_NAME}";'
    )
    if rejected_categories != [None]:
        fail("internal active-identity classifier is too broad")

    active_cache_categories = legacy_categories_for_line(
        "source/trusted_cache.cpp",
        f'return base / "{LEGACY_NAME}";',
    )
    if active_cache_categories != [None]:
        fail("internal active cache classifier is too broad")

    artifact_categories = legacy_categories_for_line(
        "tests/test-install-layout.sh",
        f"active {LEGACY_NAME} application label",
    )
    if artifact_categories != [None]:
        fail("internal artifact-test classifier is too broad")

    test_path_categories = legacy_categories_for_line(
        "tests/example.sh",
        f'runner="$tmp_dir/{LEGACY_NAME}-helper"',
    )
    if test_path_categories != [None]:
        fail("internal test-storage classifier is too broad")

    if classify_legacy_path(f"tests/{LEGACY_NAME}-cli-stub") is not None:
        fail("internal legacy-path classifier is too broad")

    if classify_legacy_path(f"LICENSES/{LEGACY_NAME}-MIT-legacy.txt") != (
        "historical-license-file"
    ):
        fail("internal historical-license path classifier self-test failed")

    rejected_allowance_cases = (
        (
            "tests/test-install-layout.sh",
            f'test -e "$DESTDIR/usr/bin/{LEGACY_NAME}-helper"',
        ),
        (
            "tests/test-app-config.sh",
            f'runner="$tmp_dir/{LEGACY_NAME}.conf-helper"',
        ),
        (
            "tests/test-app-config.sh",
            f'runner="$tmp_dir/{LEGACY_NAME}.log-helper"',
        ),
        (
            "tests/test-runtime-identity.sh",
            f'assert_not_contains "Started {LEGACY_NAME}-helper" "$output"',
        ),
        (
            "tests/test-runtime-identity.sh",
            f'assert_contains "Started {LEGACY_NAME}" "$output"',
        ),
        (
            "tests/test-localization.sh",
            f"assert_not_contains 'legacy {LEGACY_NAME}.conf-helper' \"$output\"",
        ),
        (
            "scripts/check-license-compliance.sh",
            f"v1.15.0 or later {LEGACY_NAME}-helper releases",
        ),
        (
            "scripts/check-license-compliance.sh",
            f"linked into {LEGACY_NAME}-helper",
        ),
        (
            "scripts/check-license-compliance.sh",
            f"bundled with {LEGACY_NAME}-helper",
        ),
        (
            "scripts/check-release-version.sh",
            f'expected="x{LEGACY_NAME} $version"',
        ),
        (
            "scripts/check-license-compliance.sh",
            f"x{LEGACY_NAME}-src",
        ),
        (
            "PKGBUILD",
            f"the current package is {LEGACY_NAME}",
        ),
        (
            ".github/workflows/mirror-gitlab.yml",
            f"git@gitlab.com:seekerkrt/{LEGACY_NAME}.git",
        ),
        (
            "tests/test-help-man-completion.sh",
            f"x{LEGACY_NAME}_global_options",
        ),
        (
            "tests/test-install-layout.sh",
            f"x{LEGACY_NAME}-MIT-legacy.txt",
        ),
    )
    for path, line in rejected_allowance_cases:
        if legacy_categories_for_line(path, line) != [None]:
            fail("internal active legacy classifier is too broad")

    wrong_hooks = (
        "OTHER" + "_TEST_FAKE",
        "MOGUET_INTERNAL" + "_TEST_BAD",
        "OTHER_NAMESPACE" + "_ENABLE_FAKE",
        "OTHER" + "_TEST_OBJECTS_HOOK",
    )
    for wrong_hook in wrong_hooks:
        if wrong_hook not in extract_test_hook_tokens(wrong_hook):
            fail("internal test-hook classifier self-test failed")

    infrastructure_tokens = (
        "OTHER" + "_TEST_TARGET",
        "OTHER" + "_TEST_TARGETS",
        "OTHER" + "_TEST_SRCS",
        "OTHER" + "_TEST_SUPPORT_SRCS",
        "OTHER" + "_TEST_CPPFLAGS",
        "OTHER" + "_TEST_LDLIBS",
        "OTHER" + "_TEST_OBJECT_DIR",
        "OTHER" + "_TEST_OBJECTS",
        "OTHER" + "_TEST_LINK_OBJECTS",
        "OTHER" + "_TEST_DEPS",
        "OTHER" + "_TEST_COMPILE_SIGNATURE",
        "OTHER" + "_TEST_LINK_SIGNATURE",
    )
    for infrastructure_token in infrastructure_tokens:
        if extract_test_hook_tokens(infrastructure_token):
            fail("internal test-infrastructure classifier self-test failed")


def main() -> int:
    check_classifier_contract()
    paths = repository_paths()
    texts: dict[str, str] = {}
    for path in paths:
        text = read_text(path)
        if text is not None:
            texts[path] = text

    findings: list[Finding] = []
    category_counts: Counter[str] = Counter()
    legacy_pattern = re.compile(re.escape(LEGACY_NAME), re.IGNORECASE)

    for path, required_tokens in FINAL_REPOSITORY_TOKENS.items():
        text = texts.get(path, "")
        for required_token in required_tokens:
            if required_token not in text:
                findings.append(
                    Finding(
                        "missing-final-repository-identity",
                        path,
                        0,
                        required_token,
                    )
                )

    for (path, token), expected_count in FINAL_REPOSITORY_TOKEN_COUNTS.items():
        actual_count = texts.get(path, "").count(token)
        if actual_count != expected_count:
            findings.append(
                Finding(
                    "repository-automation-identity-count",
                    path,
                    0,
                    f"{token}: expected={expected_count}, actual={actual_count}",
                )
            )

    for path, text in texts.items():
        if legacy_pattern.search(path):
            category = classify_legacy_path(path)
            if category is None:
                findings.append(Finding("unclassified-legacy-path", path, 0, path))
            else:
                category_counts[category] += 1

        for line_number, line in enumerate(text.splitlines(), start=1):
            matches = list(legacy_pattern.finditer(line))
            if not matches:
                continue
            for match in matches:
                category = classify_legacy_reference(
                    path, line, match.start(), match.end()
                )
                if category is None:
                    findings.append(
                        finding_for_line(
                            "unclassified-legacy-reference", path, line_number, line
                        )
                    )
                else:
                    category_counts[category] += 1

        if OBSOLETE_SOURCE_PREFERENCE_OVERRIDE in text:
            for line_number, line in enumerate(text.splitlines(), start=1):
                if OBSOLETE_SOURCE_PREFERENCE_OVERRIDE in line:
                    findings.append(
                        finding_for_line(
                            "obsolete-source-preference-authority",
                            path,
                            line_number,
                            line,
                        )
                    )

        if path.startswith("source/") or path in RUNTIME_AUTHORITY_PATHS:
            for line_number, line in enumerate(text.splitlines(), start=1):
                for forbidden_path in FORBIDDEN_RUNTIME_SYSTEM_PATHS:
                    if forbidden_path in line:
                        findings.append(
                            finding_for_line(
                                "forbidden-runtime-system-authority",
                                path,
                                line_number,
                                line,
                            )
                        )

    active_texts = {
        path: text for path, text in texts.items() if is_active_identity_path(path)
    }
    old_hook_pattern = re.compile(
        rf"\b{re.escape(LEGACY_UPPER)}_(?:ENABLE|TEST)_[A-Z0-9_]+\b"
    )
    removed_symbols = (
        f"{LEGACY_TITLE}GlobalOption",
        f"{LEGACY_UPPER}_GLOBAL_OPTIONS",
        f"{LEGACY_NAME}_global_option_kind",
        f"is_{LEGACY_NAME}_global_option",
        f"apply_{LEGACY_NAME}_global_option",
        f"{LEGACY_UPPER}_HAS_EXTRACTED_SOURCE_INSTALL",
        f"{LEGACY_NAME}_program_main",
    )
    removed_test_identity_pattern = re.compile(
        rf"(?:build/tests/{re.escape(LEGACY_NAME)}-|"
        rf"{re.escape(LEGACY_NAME)}-test|"
        rf"{re.escape(LEGACY_NAME)}-(?:config-test|upgrade-metadata-test|"
        rf"artifact|aur-update|edit-src|inspect|issue|multiple|package|"
        rf"pkgbuild|process|production|repository|separated|preserved|options))"
    )

    for path, text in active_texts.items():
        for line_number, line in enumerate(text.splitlines(), start=1):
            if old_hook_pattern.search(line):
                findings.append(
                    finding_for_line("old-test-hook-prefix", path, line_number, line)
                )
            if any(symbol in line for symbol in removed_symbols):
                findings.append(
                    finding_for_line("removed-internal-symbol", path, line_number, line)
                )
            if removed_test_identity_pattern.search(line):
                findings.append(
                    finding_for_line("old-active-test-identity", path, line_number, line)
                )

        if removed_test_identity_pattern.search(path):
            findings.append(Finding("old-active-test-path", path, 0, path))

    rejected_names = (
        ("rejected-romanization", REJECTED_ROMANIZATION),
        ("rejected-japanese-name", REJECTED_JAPANESE_NAME),
        ("former-name-candidate", FORMER_CANDIDATE),
    )
    for check, rejected_name in rejected_names:
        if check == "rejected-romanization":
            # POLICY(#35,#309): the lowercase French source word is legitimate
            # only as naming-origin prose. Uppercase and title-case forms remain
            # rejected alternate project spellings.
            pattern = re.compile(
                rf"\b(?:{re.escape(REJECTED_ROMANIZATION)}|"
                rf"{re.escape(REJECTED_ROMANIZATION.capitalize())})\b"
            )
        else:
            pattern = re.compile(re.escape(rejected_name), re.IGNORECASE)
        for path, text in texts.items():
            if pattern.search(path):
                findings.append(Finding(check + "-path", path, 0, path))
            for line_number, line in enumerate(text.splitlines(), start=1):
                matches = list(pattern.finditer(line))
                if not matches:
                    continue
                findings.append(finding_for_line(check, path, line_number, line))

    identity_header = texts.get("source/application_identity.hpp", "")
    command_match = re.search(r'COMMAND_NAME\s*=\s*"([^"]+)"', identity_header)
    prefix_match = re.search(
        r'ENVIRONMENT_PREFIX\s*=\s*"([A-Z0-9_]+)"', identity_header
    )
    if command_match is None or prefix_match is None:
        findings.append(
            Finding(
                "missing-application-identity-authority",
                "source/application_identity.hpp",
                0,
                "COMMAND_NAME or ENVIRONMENT_PREFIX is missing",
            )
        )
        environment_prefix = ""
    else:
        command_name = command_match.group(1)
        environment_prefix = prefix_match.group(1)
        expected_prefix = command_name.upper() + "_"
        if command_name != CURRENT_COMMAND or environment_prefix != expected_prefix:
            findings.append(
                Finding(
                    "application-identity-prefix-mismatch",
                    "source/application_identity.hpp",
                    0,
                    f"command={command_name}, prefix={environment_prefix}, "
                    f"expected={CURRENT_COMMAND}/{expected_prefix}",
                )
            )

    hook_tokens: set[str] = set()
    for path, text in active_texts.items():
        for line_number, line in enumerate(text.splitlines(), start=1):
            line_hook_tokens = extract_test_hook_tokens(line)
            hook_tokens.update(line_hook_tokens)
            for hook_token in line_hook_tokens:
                valid_prefix = environment_prefix and (
                    hook_token.startswith(environment_prefix + "ENABLE_")
                    or hook_token.startswith(environment_prefix + "TEST_")
                )
                if not valid_prefix:
                    findings.append(
                        finding_for_line(
                            "test-hook-prefix-mismatch",
                            path,
                            line_number,
                            line,
                        )
                    )
    if not hook_tokens:
        findings.append(
            Finding(
                "missing-test-hook-contract",
                "<active-source-build-tests>",
                0,
                "no ENABLE/TEST hook tokens were found",
            )
        )

    cmake_test_manifest_path = "cmake/MoguetTestTargets.cmake"
    cmake_test_manifest = texts.get(cmake_test_manifest_path, "")
    expected_target_block = re.search(
        r"set\(\s*MOGUET_EXPECTED_CPP_TEST_TARGETS\s+(.*?)\n\)",
        cmake_test_manifest,
        re.DOTALL,
    )
    required_test_targets = (
        "moguet-test",
        "moguet-root-execution-identity-test",
        "moguet-commands-inspect-test",
        "moguet-aur-update-command-test",
        "moguet-upgrade-all-command-test",
        "moguet-aur-rpc-validation-test",
        "moguet-commands-sync-test",
        "moguet-source-install-characterization-test",
        "moguet-app-config-test",
        "moguet-upgrade-baseline-metadata-test",
    )
    if expected_target_block is None:
        findings.append(
            Finding(
                "missing-moguet-cmake-test-inventory",
                cmake_test_manifest_path,
                0,
                "MOGUET_EXPECTED_CPP_TEST_TARGETS",
            )
        )
    else:
        expected_target_text = expected_target_block.group(1)
        for target_name in required_test_targets:
            target_pattern = re.compile(
                rf"^\s*{re.escape(target_name)}\s*$", re.MULTILINE
            )
            if target_pattern.search(expected_target_text) is not None:
                continue
            findings.append(
                Finding(
                    "missing-moguet-cmake-test-target",
                    cmake_test_manifest_path,
                    0,
                    target_name,
                )
            )

    required_internal_symbols = (
        "MoguetGlobalOption",
        "MOGUET_GLOBAL_OPTIONS",
        "moguet_global_option_kind",
        "apply_moguet_global_option",
        "is_moguet_global_option",
    )
    cli_authority_text = (
        texts.get("source/cli_authority.hpp", "")
        + texts.get("source/cli_parser.cpp", "")
        + texts.get("source/cli_parser.hpp", "")
    )
    for symbol in required_internal_symbols:
        if symbol not in cli_authority_text:
            findings.append(
                Finding("missing-moguet-internal-symbol", "source/cli_parser.cpp", 0, symbol)
            )

    moguet_source = texts.get("source/moguet.cpp", "")
    if 'char program[] = "moguet";' not in moguet_source:
        findings.append(
            Finding(
                "test-argv-zero-identity",
                "source/moguet.cpp",
                0,
                "test argv[0] must be moguet",
            )
        )
    source_maintenance = texts.get("source/commands_source_maintenance.cpp", "")
    cache_prompt_pattern = re.compile(
        r'"Clean \{\} build cache \(\{\}\)\?",\s*'
        r"application_identity::PROJECT_NAME,\s*"
        r"cleanup\.cache_path\(\)\.string\(\)"
    )
    if cache_prompt_pattern.search(source_maintenance) is None:
        findings.append(
            Finding(
                "moguet-cache-presentation",
                "source/commands_source_maintenance.cpp",
                0,
                "Moguet cache prompt must keep the project identity as runtime data",
            )
        )

    required_stubs = (
        "tests/stubs/moguet-test-editor",
        "tests/stubs/source-maintenance/moguet-test-editor",
    )
    for stub_path in required_stubs:
        absolute_path = REPOSITORY_ROOT / stub_path
        try:
            mode = absolute_path.lstat().st_mode
        except FileNotFoundError:
            findings.append(Finding("missing-moguet-test-stub", stub_path, 0, stub_path))
            continue
        if absolute_path.is_symlink() or not stat.S_ISREG(mode) or not mode & stat.S_IXUSR:
            findings.append(
                Finding(
                    "unsafe-moguet-test-stub",
                    stub_path,
                    0,
                    "stub must be a regular executable file",
                )
            )

    if findings:
        for finding in sorted(
            set(findings),
            key=lambda item: (item.path, item.line_number, item.check, item.line),
        ):
            location = (
                f"{finding.path}:{finding.line_number}"
                if finding.line_number > 0
                else finding.path
            )
            print(
                f"internal-identity-audit: {finding.check}: "
                f"{location}: {finding.line}",
                file=sys.stderr,
            )
        print(
            f"internal-identity-audit: failed with {len(set(findings))} finding(s)",
            file=sys.stderr,
        )
        return 1

    print(
        "internal-identity-audit: ok: "
        f"{len(hook_tokens)} test hook names use {environment_prefix}"
    )
    for category, count in sorted(category_counts.items()):
        print(f"internal-identity-audit: legacy {category}: {count}")
    print("internal-identity-audit: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
