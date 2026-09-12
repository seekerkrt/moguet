#!/usr/bin/env python3
"""Real CLI/runner/confirmation cancellation with fixture-only external effects."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parent.parent
NAMES = ("audit-a", "audit-b", "audit-c")


def snapshot(directory):
    return {
        str(path.relative_to(directory)): ("directory" if path.is_dir() else
                                         hashlib.sha256(path.read_bytes()).hexdigest())
        for path in directory.rglob("*")
    }


def run_case(binary, root, rpc_url, route):
    case = root / route.replace("-", "_")
    case.mkdir(mode=0o700)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MOGUET_TEST_")}
    env.update(LANG="C", LC_ALL="C", LANGUAGE="", no_proxy="127.0.0.1",
               NO_PROXY="127.0.0.1", PATH=f"{ROOT}/tests/stubs:/usr/bin:/bin")
    for key, directory in (("HOME", "home"), ("XDG_CONFIG_HOME", "config"),
                           ("XDG_CACHE_HOME", "cache"), ("XDG_STATE_HOME", "state")):
        path = case / directory
        path.mkdir(mode=0o700)
        env[key] = str(path)
    for name in ("git", "pacman", "pacman-conf", "makepkg", "sudo", "vercmp"):
        expected = ROOT / "tests/stubs" / name
        assert expected.is_file() and not expected.is_symlink()
        assert shutil.which(name, path=env["PATH"]) == str(expected)

    commands = case / "commands.log"
    commands.touch()
    build_cwds = case / "build-cwds.log"
    build_cwds.touch()
    inventory = case / "foreign.state"
    inventory.write_text("".join(f"{name} 0.9-1 explicit\n" for name in NAMES))
    installed = case / "installed.state"
    installed.write_text("".join(f"{name} 0.9-1\n" for name in NAMES))
    installed_after = case / "installed-after.state"
    installed_after.write_text("audit-a 1.0-1\naudit-b 0.9-1\naudit-c 0.9-1\n")
    repository = case / "repository.state"
    repository.touch()
    env.update({
        "MOGUET_TEST_COMMAND_LOG": str(commands),
        "MOGUET_TEST_MAKEPKG_CWD_LOG": str(build_cwds),
        "MOGUET_TEST_AUR_RPC_BASE_URL": rpc_url,
        "MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE": str(inventory),
        "MOGUET_TEST_PACKAGE_METADATA_STATE_FILE": str(installed),
        "MOGUET_TEST_MAKEPKG_PACKAGE_METADATA_STATE_AFTER_SUCCESS_FILE": str(installed_after),
        "MOGUET_TEST_REPOSITORY_METADATA_STATE_FILE": str(repository),
        "MOGUET_TEST_PACMAN_CONF_REPOSITORY_LIST": "core",
        "MOGUET_TEST_PACMAN_EXIT_CODE": "1",
        "MOGUET_TEST_SUDO_EXIT_CODE": "0",
        "MOGUET_TEST_MAKEPKG_EXIT_CODE": "0",
        "MOGUET_TEST_LEGACY_SUDO_STUB": str(ROOT / "tests/stubs/sudo"),
    })
    cache = case / "cache/moguet"
    cache.mkdir(mode=0o700)
    for name in NAMES:
        checkout = cache / name
        (checkout / ".git").mkdir(parents=True, mode=0o700)
        (checkout / "PKGBUILD").write_text(f"pkgname={name}\npkgver=1.0\npkgrel=1\n")
        (checkout / ".git/.moguet-test-remote-url").write_text(f"https://aur.archlinux.org/{name}.git\n")
    # Only B prompts: A completes; q at B must prevent all C execution.
    (cache / "audit-b/src").mkdir()
    before_c = snapshot(cache / "audit-c")
    completed = subprocess.run(
        [sys.executable, str(ROOT / "tests/run-with-pty.py"), "--",
         str(binary), "--noedit", "--nodiff", route],
        input=b"q\n", stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        env=env, cwd=case, timeout=40, check=False)
    output = completed.stdout.decode("utf-8", errors="replace").replace("\r", "")
    log = commands.read_text()
    try:
        assert completed.returncode == 1, f"exit {completed.returncode}"
        assert "Clean build existing build directory?" in output
        assert output.count("The current operation was cancelled at an interactive confirmation.") == 1
        assert "Building AUR PackageBase: audit-a" in output
        assert "Building AUR PackageBase: audit-b" in output
        assert "Building AUR PackageBase: audit-c" not in output
        if route != "upgrade-all":
            assert "audit-a: updated" in output
            assert "audit-b: Cancelled" in output
            assert "audit-c: not attempted" in output
        else:
            # upgrade-all presents child facts and typed attention, not the
            # standalone target summary grammar.
            assert "required child: audit-b (explicit): Cancelled" in output
            assert "  items: 3 total," in output
            details = output.split("Attention-required details:\n", 1)[1]
            cancelled_detail = details.split("  - package: audit-b\n", 1)[1].split("  - package:", 1)[0]
            assert "diagnostic: Cancelled" in cancelled_detail
            assert "reason [AUR execution]: Cancelled" in cancelled_detail
            assert "Install outcome for PackageBase audit-a: succeeded." in output
            assert "selected artifact: audit-a 1.0-1" in output
            assert "not attempted: prior work item stopped" in output
            assert "required child: audit-c (explicit): not attempted: prior work item stopped" in output
        for forbidden in ("Internal inconsistency:", "result is inconsistent",
                          "metadata failure", "filtered AUR preparation failed",
                          "AUR preparation issue:", "reduction issue:",
                          "AUR update: completed"):
            assert forbidden not in output, forbidden
        lines = log.splitlines()
        installs = [line for line in lines if line.startswith("sudo pacman -U ")]
        assert len(installs) == 1 and "audit-a-1.0-1" in installs[0]
        assert sum(line.startswith("makepkg -sc") for line in lines) == 1
        assert build_cwds.read_text().splitlines()
        assert all(Path(line).name == "audit-a" for line in build_cwds.read_text().splitlines())
        assert sum(line == "git fetch origin" for line in lines) == 2
        assert sum(line.startswith("sudo pacman -Syu") for line in lines) == (route != "upgrade-aur")
        assert not any(line.startswith("sudo pacman -R") for line in lines)
        assert installed.read_bytes() == installed_after.read_bytes(), "A update was lost/rolled back"
        assert snapshot(cache / "audit-c") == before_c, "C workspace changed"
    except AssertionError:
        print(f"FAIL production cancellation: {route}\n{output}\nCOMMANDS\n{log}", file=sys.stderr)
        raise
    print(f"PASS production cancellation: {route}: A updated / B cancelled / C unattempted; exit 1; no later mutation")


def main():
    binary = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="moguet-partial-cancel-") as directory:
        root = Path(directory)
        fixture = root / "rpc.json"
        fixture.write_text(json.dumps({"packages": {
            name: {"Name": name, "PackageBase": name, "Version": "1.0-1"}
            for name in NAMES}}))
        port = root / "port"
        server = subprocess.Popen([sys.executable, str(ROOT / "tests/aur_rpc_fixture_server.py"),
                                   str(fixture), str(port)])
        try:
            deadline = time.monotonic() + 5
            while not port.exists():
                if server.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("RPC fixture did not start")
                time.sleep(0.02)
            rpc_url = f"http://127.0.0.1:{port.read_text()}/rpc/"
            for route in ("upgrade-aur", "-Syu", "upgrade-all"):
                run_case(binary, root, rpc_url, route)
        finally:
            server.terminate()
            server.wait(timeout=5)


if __name__ == "__main__":
    main()
