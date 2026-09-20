#!/usr/bin/env python3
"""Real public remote build/collector/interaction/executor; external tools/DB stubbed."""
import json
import os
from pathlib import Path
import pty
import select
import subprocess
import sys
import tempfile
import time

repo = Path(__file__).resolve().parents[1]
binary, typed_binary = map(lambda p: str(Path(p).resolve()), sys.argv[1:])


def require(condition, message):
    if not condition:
        raise AssertionError(message)


with tempfile.TemporaryDirectory(prefix="moguet-rmdeps-") as temporary:
    root = Path(temporary)
    fixture = root / "rpc.json"
    fixture.write_text(json.dumps({"packages": {"cleanup-root": {
        "Name": "cleanup-root", "PackageBase": "cleanup-root", "Version": "1.0-1",
        "Depends": [], "MakeDepends": ["dep-a", "dep-b"]}}}))
    port = root / "port"
    server = subprocess.Popen([sys.executable, str(repo / "tests/aur_rpc_fixture_server.py"),
                               str(fixture), str(port)], stdout=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + 10
        while not port.exists():
            require(server.poll() is None and time.monotonic() < deadline, "RPC fixture startup failed")
            time.sleep(.02)
        base = "base-devel 1.0-1 explicit base-devel x86_64 unrelated-tool\n"
        deps = "dep-a 1.0-1 dependency dep-a x86_64\ndep-b 1.0-1 dependency dep-b x86_64\n"
        post = base + deps + "cleanup-root 1.0-1 explicit cleanup-root x86_64\n"

        def run(name, *, requested=True, answer="y\n", tty=True, no_confirm=False,
                pre=base, after=post, change=None, hold="", remove_exit=0,
                make_exit=0, install_exit=0, packagelist_exit=0, typed=False, dry=False, route=None):
            case = root / name
            case.mkdir()
            env = {k: v for k, v in os.environ.items() if not k.startswith("MOGUET_TEST_")}
            for key, directory in [("HOME", "home"), ("XDG_CONFIG_HOME", "config"),
                                   ("XDG_STATE_HOME", "state"), ("XDG_CACHE_HOME", "cache")]:
                path = case / directory
                path.mkdir(mode=0o700)
                env[key] = str(path)
            state = case / "installed"
            state.write_text(pre)
            after_file = case / "after"
            after_file.write_text(after)
            dependency_file = case / "dependencies"
            dependency_file.write_text(base + deps)
            repository_state = case / "repository"
            repository_state.write_text("core dep-a 1 1 dep-a\ncore dep-b 1 1 dep-b\n")
            log = case / "commands"
            log.touch()
            env.update({
                "LANG": "C", "LC_ALL": "C", "LANGUAGE": "C",
                "PATH": f"{repo}/tests/stubs:/usr/bin:/bin",
                "MOGUET_TEST_REPOSITORY_ROOT": str(repo),
                "MOGUET_TEST_AUR_RPC_BASE_URL": f"http://127.0.0.1:{port.read_text().strip()}/rpc/",
                "MOGUET_TEST_COMMAND_LOG": str(log),
                "MOGUET_TEST_LEGACY_SUDO_STUB": str(repo / "tests/stubs/sudo"),
                "MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG": str(log),
                "MOGUET_TEST_CLEANUP_METADATA_STATE_FILE": str(state),
                "MOGUET_TEST_CLEANUP_DEPENDENCY_STATE_FILE": str(dependency_file),
                "MOGUET_TEST_PACKAGE_METADATA_STATE_FILE": str(state),
                "MOGUET_TEST_REPOSITORY_METADATA_STATE_FILE": str(repository_state),
                "MOGUET_TEST_PACMAN_CONF_REPOSITORY_LIST": "core",
                "MOGUET_TEST_PACMAN_EXIT_CODE": "1",
                "MOGUET_TEST_SUDO_EXIT_CODE": str(install_exit),
                "MOGUET_TEST_CLEANUP_REMOVAL_EXIT_CODE": str(remove_exit),
                "MOGUET_TEST_MAKEPKG_EXIT_CODE": str(make_exit),
                "MOGUET_TEST_MAKEPKG_PACKAGELIST_EXIT_CODE": str(packagelist_exit),
                "MOGUET_TEST_MAKEPKG_PACKAGE_METADATA_STATE_AFTER_SUCCESS_FILE": str(after_file),
                "MOGUET_TEST_HOLDPKG": hold,
            })
            # Every possible privileged/external tool is an exact fixture executable.
            for tool in ["sudo", "pacman", "pacman-conf", "makepkg", "git"]:
                require((repo / "tests/stubs" / tool).is_file(), f"missing stub {tool}")
            if typed:
                args = [typed_binary, "remote-rmdeps"]
                if no_confirm:
                    args.append("noconfirm")
                elif not requested:
                    args.append("unrequested")
            else:
                args = ([binary] + route) if route else [binary, "--noedit", "--nodiff", "build", "cleanup-root"]
                if requested:
                    args.append("--rmdeps")
                if no_confirm:
                    args.append("--noconfirm")
                if dry:
                    args.append("--dry-run")
            if tty:
                master, slave = pty.openpty()
                process = subprocess.Popen(args, env=env, stdin=slave, stdout=slave, stderr=slave)
                os.close(slave)
                output = b""
                sent = False
                deadline = time.monotonic() + 30
                try:
                    while True:
                        require(time.monotonic() < deadline, f"{name}: timeout: {output.decode(errors='replace')}")
                        if select.select([master], [], [], .05)[0]:
                            try:
                                chunk = os.read(master, 65536)
                            except OSError:
                                break
                            if not chunk:
                                break
                            output += chunk
                        if b"Remove build dependencies? [y/N]" in output and not sent:
                            if change:
                                state.write_text(change)
                            os.write(master, b"\x04" if answer is None else answer.encode())
                            sent = True
                        if process.poll() is not None:
                            # Drain the PTY to EOF in the next iterations.
                            continue
                    status = process.wait(timeout=5)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
                    os.close(master)
                output = output.decode(errors="replace").replace("\r", "")
            else:
                result = subprocess.run(args, env=env, input=answer or "", text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
                status, output = result.returncode, result.stdout
            commands = log.read_text().splitlines()
            removals = [line for line in commands if line.startswith("sudo pacman -R")]
            build_start = next((i for i, line in enumerate(commands) if line.startswith("git clone ")), len(commands))
            install = next((i for i, line in enumerate(commands) if line.startswith("sudo pacman -U ")), len(commands))
            baseline = commands[:build_start].count("alpm cleanup snapshot")
            fresh_queries = commands[install + 1:].count("pacman-conf --verbose RootDir DBPath")
            require(baseline == (1 if requested and not dry and route is None else 0), f"{name}: baseline gate {commands}")
            require(fresh_queries == commands.count("pacman-conf HoldPkg"), f"{name}: fresh query without approval {commands}")
            print(f"{name}: exit={status} baseline={baseline} fresh={fresh_queries} local-cache={commands.count('alpm cleanup snapshot')} "
                  f"HoldPkg={commands.count('pacman-conf HoldPkg')} removal={len(removals)}")
            return status, output, commands, removals

        def checked(name, expected=0, **kwargs):
            result = run(name, **kwargs)
            require(result[0] == expected, f"{name}: expected exit {expected}, got {result[0]}\n{result[1]}\n{result[2]}")
            return result

        _, out, commands, removal = checked("yes")
        require(removal == ["sudo pacman -R --noconfirm -- dep-a dep-b"], f"exact one-shot argv: {removal}\n{out}")
        require(out.count("Remove build dependencies? [y/N]") == 1, out)
        require(out.index("Install outcome for PackageBase cleanup-root: succeeded.") < out.index("Assessed build dependencies"), out)
        require(out.index("Assessed build dependencies") < out.index("Remove build dependencies?") < out.index("Dependency cleanup removed"), out)
        require(commands.index(next(x for x in commands if x.startswith("sudo pacman -U"))) < commands.index("pacman-conf HoldPkg"), commands)

        _, out, commands, removal = checked("unrequested", requested=False)
        require(not removal and "cleanup" not in out.lower().replace("cleanup-root", ""), out)
        # PackageMetadataSession preloads the local cache for ordinary artifact
        # install too. Only that one read is allowed, after makepkg; no baseline
        # before build, post-success candidate/policy reads, or fresh removal read.
        require(commands.count("alpm cleanup snapshot") == 1 and "pacman-conf HoldPkg" not in commands, commands)
        require(commands.index("alpm cleanup snapshot") > commands.index("makepkg -sc"), commands)
        require("alpm query base-devel" not in commands, commands)

        for name, answer in [("enter", "\n"), ("no", "n\n"), ("cancel", "q\n"), ("eof", None)]:
            _, out, commands, removal = checked(name, answer=answer)
            require(not removal and "pacman-conf HoldPkg" not in commands, commands)
            require(out.count("Remove build dependencies? [y/N]") == 1, out)
            require("declined" in out if name in ("enter", "no") else "cancelled" in out, out)

        for name, kwargs, reason in [("noconfirm", {"no_confirm": True}, "--noconfirm does not approve"),
                                     ("non-tty", {"tty": False}, "input is not interactive")]:
            _, out, commands, removal = checked(name, expected=1, **kwargs)
            require(not removal and "pacman-conf HoldPkg" not in commands and reason in out, out)
            require("Remove build dependencies?" not in out, out)

        for name, kwargs in [("build-failure", {"make_exit": 7}),
                             ("artifact-failure", {"packagelist_exit": 8}),
                             ("install-failure", {"install_exit": 9})]:
            _, out, commands, removal = checked(name, expected=1, **kwargs)
            require(not removal and "pacman-conf HoldPkg" not in commands and
                    "Assessed build dependencies" not in out and "Remove build dependencies?" not in out, out)

        for name, kwargs in [("pre-existing", {"pre": base + deps}),
                             ("absent-after", {"after": base + "cleanup-root 1.0-1 explicit cleanup-root x86_64\n"})]:
            _, out, commands, removal = checked(name, **kwargs)
            require("nothing to remove" in out and "Remove build dependencies?" not in out and not removal, out)
            require("pacman-conf HoldPkg" not in commands, commands)

        _, out, commands, removal = checked("blocked", expected=1, after=post.replace("dep-a 1.0-1", "dep-a 2.0-1"))
        require(not removal and "cleanup blocked" in out and "Remove build dependencies?" not in out, out)
        require("pacman-conf HoldPkg" not in commands, commands)

        _, out, commands, removal = checked("reason-change", change=post.replace("dep-a 1.0-1 dependency", "dep-a 1.0-1 explicit"))
        require(removal == ["sudo pacman -R --noconfirm -- dep-b"] and "dep-a: install reason changed" in out, out)
        _, out, commands, removal = checked("holdpkg", hold="dep-a")
        require(removal == ["sudo pacman -R --noconfirm -- dep-b"] and "dep-a: protected" in out, out)
        _, out, commands, removal = checked("all-held", hold="dep-*")
        require(not removal and "no candidates remain ready" in out, out)
        _, out, commands, removal = checked("removal-failure", expected=1, remove_exit=17, typed=True)
        require(len(removal) == 1 and "typed-build-success=1" in out and "typed-execution=2" in out, out)
        require("Attempted: dep-a, dep-b. Exit status: 17." in out, out)
        _, out, commands, removal = checked("dry-run", dry=True)
        require(not removal and not any(x.startswith(("sudo ", "makepkg ")) for x in commands), commands)
        require("pacman-conf HoldPkg" not in commands and "alpm cleanup snapshot" not in commands, commands)
        for name, route in [
            ("unsupported-local", ["build", "--local", str(root / "not-inspected")]),
            ("unsupported-source-sync", ["-S", "--aur", "cleanup-root"]),
            ("unsupported-upgrade-aur", ["upgrade-aur"]),
            ("unsupported-upgrade-all", ["upgrade-all"]),
        ]:
            _, out, commands, removal = checked(name, expected=1, route=route)
            require(not removal and "--rmdeps" in out and "Remove build dependencies?" not in out, out)
            require(not any(x.startswith(("sudo ", "makepkg ", "git clone ")) for x in commands), commands)
        _, out, commands, removal = checked("pacman-only", route=["-S", "--repo", "official"])
        require(not removal and "sudo pacman -S official" in commands and "--rmdeps" not in "\n".join(commands), commands)
        print("PASS: public remote AUR --rmdeps lifecycle, typed results, request/failure gates, exact removal")
    finally:
        server.terminate()
        server.wait(timeout=5)
