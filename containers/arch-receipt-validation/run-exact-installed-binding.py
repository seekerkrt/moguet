#!/usr/bin/python3

import os
from pathlib import Path
import subprocess
import sys


STATE_ROOT = Path("/var/lib/moguet-exact-installed-binding")
DATABASE_ROOT = STATE_ROOT / "db"
FIXTURE = Path("build/cmake-receipt-testing/tests/evaluated-devel-source-artifact-transport-test").resolve()


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"exact-installed-binding-container: {message}", file=sys.stderr)
        raise SystemExit(1)


def main(publication_check=None) -> None:
    # Guard before config/database writes. The Make lane supplies an anonymous
    # volume; it never bind-mounts a host checkout or host package database.
    require(os.geteuid() == 0 and Path("/.dockerenv").is_file(), "disposable Docker root runner required")
    require(STATE_ROOT.is_mount() and not STATE_ROOT.is_symlink(), "isolated anonymous volume is missing")
    require(not any(STATE_ROOT.iterdir()), "acceptance requires a fresh empty volume")
    require(FIXTURE.is_file(), "canonical Slice 5 fixture target is missing")
    for path in (DATABASE_ROOT / "local", STATE_ROOT / "cache"):
        path.mkdir(parents=True, mode=0o755)
    (DATABASE_ROOT / "local/ALPM_DB_VERSION").write_text("9\n")
    Path("/etc/pacman.conf").write_text(
        "[options]\nRootDir = /\n"
        f"DBPath = {DATABASE_ROOT}\n"
        f"LogFile = {STATE_ROOT / 'pacman.log'}\n"
        f"CacheDir = {STATE_ROOT / 'cache'}\n"
        "Architecture = auto\nSigLevel = Never\nLocalFileSigLevel = Never\n"
    )
    environment = dict(os.environ)
    environment["MOGUET_EXACT_INSTALLED_ACCEPTANCE"] = "isolated-container"
    result = subprocess.run(
        ["/usr/bin/runuser", "-u", "moguet-validation", "--", str(FIXTURE), "--installed-devel-publication" if publication_check else "--installed-exact-binding"],
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    print(result.stdout, end="")
    require(result.returncode == 0, f"actual Slice 5 fixture failed: {result.returncode}")
    records = [line.split("\t") for line in result.stdout.splitlines() if line.startswith("S5B-INSTALLED\t")]
    require(len(records) == 4, "missing actual transaction acceptance records")
    require([row[1] for row in records] == ["first-install", "upgrade", "same-version-reinstall", "downgrade"], "wrong acceptance order")
    require([row[2] for row in records] == ["Install", "Upgrade", "Upgrade", "Upgrade"], "wrong hook operation semantics")
    require(records[0][3] == records[3][3] and records[1][3] == records[2][3] and records[0][3] != records[1][3], "version fixture did not exercise Upgrade/reinstall/downgrade")
    require(len({row[5] for row in records}) == 4, "real record generation did not change for every transaction")
    require(all(len(row[4]) == 64 for row in records), "raw built/installed MTREE equality evidence missing")
    if publication_check:
        publication_check(result.stdout, records)
    else:
        final_records = [line.split("\t") for line in result.stdout.splitlines() if line.startswith("S5C-INSTALLED\t")]
        require(len(final_records) == 4 and [row[1] for row in final_records] == [row[1] for row in records], "missing final proof records")
        require(all(row[2:] == ["Complete", "cleanup-Complete", "publication-none"] for row in final_records), "final proof/cleanup/publication contract failed")
        print("exact-installed-binding-container: Install/Upgrade/reinstall/downgrade, raw MTREE, fresh live mint, final proof PASS")
        print("exact-installed-binding-container: publication=none; XDG provenance write=none")
    print("exact-installed-binding-container: network=none; database=anonymous volume; host package DB unavailable")


if __name__ == "__main__":
    main()
