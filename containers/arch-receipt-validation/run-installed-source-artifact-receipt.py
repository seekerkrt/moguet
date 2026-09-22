#!/usr/bin/python3

import fcntl
import hashlib
import os
import pathlib
import select
import stat
import subprocess
import sys
import time


HELPER = "/usr/libexec/moguet/moguet-source-artifact-install-helper"
SELECTED_HELPER = "/usr/libexec/moguet/moguet-alpm-receipt-helper"
SELECTED_OWNER = "selected-repository-provider"
SOURCE_OWNER = "source-artifact-install"
PACKAGE_ROOT = pathlib.Path("/home/moguet-validation/receipt-packages")
STATE_ROOT = pathlib.Path("/run/moguet/source-artifact-installs")
TRANSPORT_FIXTURE = "/home/moguet-validation/work/moguet-receipt/build/cmake-receipt-testing/tests/moguet-source-artifact-install-installed-fixture"

F_ADD_SEALS = getattr(fcntl, "F_ADD_SEALS", 1033)
F_GET_SEALS = getattr(fcntl, "F_GET_SEALS", 1034)
F_SEAL_SEAL = getattr(fcntl, "F_SEAL_SEAL", 0x0001)
F_SEAL_SHRINK = getattr(fcntl, "F_SEAL_SHRINK", 0x0002)
F_SEAL_GROW = getattr(fcntl, "F_SEAL_GROW", 0x0004)
F_SEAL_WRITE = getattr(fcntl, "F_SEAL_WRITE", 0x0008)
REQUIRED_SEALS = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE


def fail(message: str) -> None:
    print(
        f"source-artifact-receipt-installed-validation: {message}",
        file=sys.stderr,
    )
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def package_path(filename: str) -> pathlib.Path:
    path = PACKAGE_ROOT / filename
    require(path.is_file(), f"missing package fixture: {filename}")
    return path


def token(character: str) -> str:
    return character * 64


def sealed_stream(paths: list[pathlib.Path]) -> int:
    descriptor = os.memfd_create(
        "moguet-source-artifact-installed-fixture",
        os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING,
    )
    try:
        for path in paths:
            with path.open("rb") as source:
                while data := source.read(64 * 1024):
                    offset = 0
                    while offset < len(data):
                        offset += os.write(descriptor, data[offset:])
        fcntl.fcntl(descriptor, F_ADD_SEALS, REQUIRED_SEALS)
        require(
            fcntl.fcntl(descriptor, F_GET_SEALS) & REQUIRED_SEALS
            == REQUIRED_SEALS,
            "fixture memfd did not retain all required seals",
        )
        os.lseek(descriptor, 0, os.SEEK_SET)
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def parse_fixed_response(output: str, header: str, expected_token: str) -> list[str]:
    require(output.endswith("\n"), f"{header} response has no final newline")
    lines = output.splitlines()
    require(lines and lines[0] == header, f"unexpected {header} response header")
    require(f"TOKEN\t{expected_token}" in lines, f"{header} token mismatch")
    require(f"OWNER\t{SOURCE_OWNER}" in lines, f"{header} owner mismatch")
    require(lines[-1] == "END", f"{header} response has no END record")
    return lines


def prepare(
    transaction_token: str,
    artifacts: list[tuple[int, str, str, str, str, pathlib.Path]],
    *,
    needed: bool = False,
    no_confirm: bool = True,
    directive: str = "AsDependency",
    expect_success: bool = True,
) -> tuple[str, list[str]] | None:
    arguments = [
        HELPER,
        "prepare",
        transaction_token,
        artifacts[0][3],
        directive,
        "1" if needed else "0",
        "1" if no_confirm else "0",
        "--",
    ]
    paths: list[pathlib.Path] = []
    for artifact_index, name, version, package_base, architecture, path in artifacts:
        arguments.extend(
            [
                str(artifact_index),
                name,
                version,
                package_base,
                architecture,
                str(path.stat().st_size),
                "0",
                hashlib.sha256(path.read_bytes()).hexdigest(),
                "-",
            ]
        )
        paths.append(path)

    descriptor = sealed_stream(paths)
    try:
        result = subprocess.run(
            arguments,
            stdin=descriptor,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
            env={"PATH": "/usr/bin", "LC_ALL": "C"},
        )
    finally:
        os.close(descriptor)
    if not expect_success:
        require(result.returncode != 0, "invalid source prepare unexpectedly succeeded")
        return None
    require(
        result.returncode == 0,
        f"source prepare failed: {result.stderr.strip()}",
    )
    lines = parse_fixed_response(
        result.stdout,
        "MOGUET-SOURCE-ARTIFACT-PREPARE-RESPONSE\t2",
        transaction_token,
    )
    hook_lines = [line.removeprefix("HOOKDIR\t") for line in lines if line.startswith("HOOKDIR\t")]
    staged_lines = [line.split("\t", 2) for line in lines if line.startswith("STAGED\t")]
    require(len(hook_lines) == 1, "prepare response hookdir cardinality changed")
    require(len(staged_lines) == len(artifacts), "prepare response artifact cardinality changed")
    expected_hook = f"{STATE_ROOT}/active/{transaction_token}/hooks"
    require(hook_lines[0] == expected_hook, "prepare returned an unexpected hookdir")
    staged_paths: list[str] = []
    for ordinal, (record, artifact) in enumerate(zip(staged_lines, artifacts, strict=True)):
        expected_path = f"{STATE_ROOT}/active/{transaction_token}/artifacts/artifact-{ordinal}.pkg.tar.zst"
        require(record == ["STAGED", str(artifact[0]), expected_path], "prepare returned an unexpected staged path")
        staged_paths.append(expected_path)
    return expected_hook, staged_paths


def consume(transaction_token: str) -> tuple[str, list[str]]:
    result = subprocess.run(
        [HELPER, "consume", transaction_token],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        env={"PATH": "/usr/bin", "LC_ALL": "C"},
    )
    require(result.returncode == 0, f"consume failed: {result.stderr.strip()}")
    lines = parse_fixed_response(
        result.stdout,
        "MOGUET-SOURCE-ARTIFACT-RECEIPT\t2",
        transaction_token,
    )
    states = [line.removeprefix("STATE\t") for line in lines if line.startswith("STATE\t")]
    installs = [line.removeprefix("INSTALL\t") for line in lines if line.startswith("INSTALL\t")]
    require(len(states) == 1, "receipt state cardinality changed")
    return states[0], installs


def abort(transaction_token: str) -> None:
    result = subprocess.run(
        [HELPER, "abort", transaction_token],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        env={"PATH": "/usr/bin", "LC_ALL": "C"},
    )
    require(result.returncode == 0, f"abort failed: {result.stderr.strip()}")


def run_transaction(
    transaction_token: str,
    artifacts: list[tuple[int, str, str, str, str, pathlib.Path]],
    *,
    needed: bool = False,
    expect_pacman_success: bool = True,
) -> tuple[str, list[str]] | None:
    prepared = prepare(transaction_token, artifacts, needed=needed)
    require(prepared is not None, "valid prepare returned no state")
    # The privileged helper owns final identity reproof and the adjacent
    # archive/signature pathname handoff; this fixture must not bypass it.
    command = [HELPER, "execute", transaction_token]
    result = subprocess.run(
        command,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
        env={"PATH": "/usr/bin", "LC_ALL": "C"},
    )
    if expect_pacman_success:
        require(result.returncode == 0, "pacman -U fixture transaction failed")
        return consume(transaction_token)
    require(result.returncode != 0, "failing pacman fixture unexpectedly succeeded")
    receipt_path = STATE_ROOT / "active" / transaction_token / "receipt"
    require(not receipt_path.exists(), "failed transaction published a receipt")
    abort(transaction_token)
    return None


def package_version(name: str) -> str | None:
    result = subprocess.run(
        ["/usr/bin/pacman", "-Q", name],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        check=False,
        env={"PATH": "/usr/bin", "LC_ALL": "C"},
    )
    if result.returncode != 0:
        return None
    fields = result.stdout.strip().split()
    require(len(fields) == 2 and fields[0] == name, "pacman -Q output changed")
    return fields[1]


def run_production_transport(
    invocation: str,
    work_item_index: int,
    artifact: tuple[int, str, str, str, str, pathlib.Path],
    *,
    needed: bool = False,
) -> tuple[str, str, str, list[str], int | None]:
    _, name, version, package_base, architecture, path = artifact
    command = [
        "/usr/bin/runuser",
        "-u",
        "moguet-validation",
        "--",
        TRANSPORT_FIXTURE,
        invocation,
        str(work_item_index),
        package_base,
        name,
        version,
        architecture,
        str(path),
    ]
    if needed:
        command.append("--needed")
    result = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        env={
            "PATH": "/usr/bin",
            "LC_ALL": "C",
            "HOME": "/home/moguet-validation",
            "XDG_CACHE_HOME": "/home/moguet-validation/.cache",
            "XDG_CONFIG_HOME": "/home/moguet-validation/.config",
            "XDG_STATE_HOME": "/home/moguet-validation/.local/state",
        },
    )
    require(
        result.returncode == 0,
        f"production transport fixture failed: {result.stderr.strip()}",
    )
    lines = result.stdout.splitlines()

    def one(prefix: str) -> str:
        values = [line.removeprefix(prefix) for line in lines if line.startswith(prefix)]
        require(len(values) == 1, f"production fixture {prefix!r} cardinality changed")
        return values[0]

    status = one("STATUS\t")
    operation = one("OPERATION\t")
    evidence = one("EVIDENCE\t")
    causal = one("CAUSAL\t")
    installs = [line.removeprefix("INSTALL\t") for line in lines if line.startswith("INSTALL\t")]
    pacman_values = [line.removeprefix("PACMAN\t") for line in lines if line.startswith("PACMAN\t")]
    require(len(pacman_values) <= 1, "production fixture PACMAN cardinality changed")
    pacman_status = int(pacman_values[0]) if pacman_values else None
    require(lines[-1] == "END", "production fixture has no END record")
    if pacman_status == 0:
        require(
            operation == "Succeeded",
            "successful pacman transaction lost its independent operation result",
        )
    else:
        require(
            operation == "Unavailable",
            "failed or unknown transaction published operation success",
        )
    return status, evidence, causal, installs, pacman_status


def run_cleanup_lifecycle(
    artifact: tuple[int, str, str, str, str, pathlib.Path],
    scenario: str = "positive",
) -> tuple[str, str, str, str]:
    _, name, version, package_base, architecture, path = artifact
    result = subprocess.run(
        [
            "/usr/bin/runuser",
            "-u",
            "moguet-validation",
            "--",
            TRANSPORT_FIXTURE,
            "--cleanup-lifecycle",
            scenario,
            str(path),
            name,
            package_base,
            version,
            architecture,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        env={
            "PATH": "/usr/bin",
            "LC_ALL": "C",
            "HOME": "/home/moguet-validation",
            "XDG_CACHE_HOME": "/home/moguet-validation/.cache",
            "XDG_CONFIG_HOME": "/home/moguet-validation/.config",
            "XDG_STATE_HOME": "/home/moguet-validation/.local/state",
        },
    )
    require(
        result.returncode == 0,
        f"cleanup lifecycle fixture failed: {result.stderr.strip()}",
    )
    lines = result.stdout.splitlines()

    def one(prefix: str) -> str:
        values = [
            line.removeprefix(prefix)
            for line in lines
            if line.startswith(prefix)
        ]
        require(
            len(values) == 1,
            f"cleanup lifecycle {prefix!r} cardinality changed",
        )
        return values[0]

    require(lines[-1] == "END", "cleanup lifecycle fixture has no END record")
    return (
        one("LIFECYCLE\t"),
        one("CLASSIFICATION\t"),
        one("PACKAGE\t"),
        one("WORK_ITEMS\t"),
    )


def run_cleanup_authority_scenario(
    scenario: str,
    lifecycle_dependency: pathlib.Path,
) -> None:
    require(
        scenario in {"positive", "later-failed", "later-not-attempted"},
        f"unknown cleanup-authority scenario: {scenario}",
    )
    require(
        package_version("moguet-receipt-dependency") is None,
        "cleanup lifecycle dependency was pre-existing",
    )
    lifecycle, classification, lifecycle_package, work_items = (
        run_cleanup_lifecycle(
            (
                0,
                "moguet-receipt-dependency",
                "1-1",
                "moguet-receipt-dependency",
                "any",
                lifecycle_dependency,
            ),
            scenario,
        )
    )
    expected = {
        "positive": ("Complete", "Eligible", "Succeeded,Succeeded"),
        "later-failed": (
            "NotCompleted",
            "NotProduced",
            "Succeeded,Failed",
        ),
        "later-not-attempted": (
            "NotCompleted",
            "NotProduced",
            "Succeeded,Failed,NotAttempted",
        ),
    }[scenario]
    require(
        (lifecycle, classification, work_items) == expected
        and lifecycle_package == "moguet-receipt-dependency",
        f"cleanup-authority {scenario} composition result changed",
    )
    require(
        package_version("moguet-receipt-dependency") == "1-1",
        f"cleanup-authority {scenario} actual dependency Install did not persist",
    )


def expect_rejection(arguments: list[str], label: str) -> None:
    result = subprocess.run(
        arguments,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
        env={"PATH": "/usr/bin", "LC_ALL": "C"},
    )
    require(result.returncode != 0, f"{label} unexpectedly succeeded")


def run_cleanup_execution(dependency: pathlib.Path) -> None:
    """Current collector/approval/executor with actual isolated pacman effects."""
    name = "moguet-receipt-dependency"
    target = "moguet-receipt-target"
    require(package_version(name) is None and package_version(target) is None,
            "cleanup execution needs a fresh container")
    environment = {"PATH": "/usr/bin", "LC_ALL": "C"}

    def inventory() -> tuple[bytes, bytes]:
        return tuple(subprocess.check_output(["/usr/bin/pacman", option], env=environment)
                     for option in ("-Q", "-Qqe"))

    before = inventory()
    config = pathlib.Path("/etc/pacman.conf")
    original = config.read_bytes()
    process = subprocess.Popen(
        ["/usr/bin/runuser", "-u", "moguet-validation", "--", TRANSPORT_FIXTURE,
         "--cleanup-lifecycle", "execute", str(dependency), name, name, "1-1", "any"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        env=environment | {"HOME": "/home/moguet-validation",
                           "XDG_CACHE_HOME": "/home/moguet-validation/.cache",
                           "XDG_CONFIG_HOME": "/home/moguet-validation/.config",
                           "XDG_STATE_HOME": "/home/moguet-validation/.local/state"},
    )
    output = b""

    def wait_for(marker: bytes) -> None:
        nonlocal output
        deadline = time.monotonic() + 60
        while marker not in output:
            require(time.monotonic() < deadline, f"cleanup barrier timeout: {output!r}")
            if select.select([process.stdout], [], [], .1)[0]:
                chunk = os.read(process.stdout.fileno(), 65536)
                require(bool(chunk), f"cleanup fixture ended before {marker!r}: {output!r}")
                output += chunk

    try:
        for step in ("protected", "still-required", "remove"):
            wait_for(f"READY\t{step}\n".encode())
            require(package_version(name) == "1-1", "collector dependency install missing")
            if step == "protected":
                require(b"[options]" in original, "pacman options section absent")
                config.write_bytes(original.replace(b"[options]", b"[options]\nHoldPkg = " + name.encode(), 1))
            elif step == "still-required":
                config.write_bytes(original)
                subprocess.run(["/usr/bin/pacman", "-U", "--noconfirm", "--",
                                str(package_path("moguet-receipt-target-1-1-any.pkg.tar.zst"))],
                               env=environment, check=True, stdout=subprocess.DEVNULL)
            else:
                subprocess.run(["/usr/bin/pacman", "-R", "--noconfirm", "--", target],
                               env=environment, check=True, stdout=subprocess.DEVNULL)
            process.stdin.write(b"y\n")
            process.stdin.flush()
            wait_for(f"EXECUTION\t{step}\tPASS\n".encode())
            require(package_version(name) == (None if step == "remove" else "1-1"),
                    f"actual package state differs after {step}")
        require(process.wait(timeout=10) == 0, f"cleanup execution failed: {output!r}")
        require(inventory() == before, "cleanup changed unrelated package versions/reasons")
        print("cleanup-execution: real collector -> explicit approval -> fresh Protected/StillRequired -> exact removal PASS")
    finally:
        config.write_bytes(original)
        if process.poll() is None:
            process.kill()
            process.wait()
        process.stdin.close()
        process.stdout.close()


def main() -> None:
    require(os.geteuid() == 0, "installed fixture must run as container root")
    for helper in (HELPER, SELECTED_HELPER):
        metadata = os.stat(helper, follow_symlinks=False)
        require(stat.S_ISREG(metadata.st_mode), f"{helper} is not regular")
        require(metadata.st_uid == 0 and stat.S_IMODE(metadata.st_mode) == 0o755, f"{helper} provenance/mode changed")

    v1 = package_path("moguet-source-receipt-single-1-1-any.pkg.tar.zst")
    v2 = package_path("moguet-source-receipt-single-2-1-any.pkg.tar.zst")
    failure = package_path("moguet-source-receipt-failure-1-1-any.pkg.tar.zst")
    multi_a = package_path("moguet-source-receipt-multi-a-1-1-any.pkg.tar.zst")
    multi_b = package_path("moguet-source-receipt-multi-b-1-1-any.pkg.tar.zst")
    lifecycle_dependency = package_path(
        "moguet-receipt-dependency-1-1-any.pkg.tar.zst"
    )

    if len(sys.argv) == 3 and sys.argv[1] == "--cleanup-authority-scenario":
        run_cleanup_authority_scenario(sys.argv[2], lifecycle_dependency)
        print(f"cleanup-authority {sys.argv[2]} composition checks passed")
        return
    require(len(sys.argv) == 1, "unexpected source-artifact fixture arguments")

    single_v1 = [(0, "moguet-source-receipt-single", "1-1", "moguet-source-receipt-single", "any", v1)]
    single_v2 = [(0, "moguet-source-receipt-single", "2-1", "moguet-source-receipt-single", "any", v2)]

    run_cleanup_execution(lifecycle_dependency)
    run_cleanup_authority_scenario("positive", lifecycle_dependency)

    mismatch_token = token("a")
    prepare(
        mismatch_token,
        [(0, "moguet-source-receipt-single", "9-1", "moguet-source-receipt-single", "any", v1)],
        expect_success=False,
    )
    require(package_version("moguet-source-receipt-single") is None, "metadata mismatch mutated package DB")
    require(not (STATE_ROOT / "active" / mismatch_token).exists(), "metadata mismatch left active state")

    status, evidence, causal, installs, pacman_status = run_production_transport(
        "installed-invocation", 0, single_v1[0]
    )
    require(status == "Complete" and evidence == "Complete" and causal == "Established", "absent -> v1 did not establish causal evidence")
    require(installs == ["moguet-source-receipt-single"] and pacman_status == 0, "absent -> v1 actual Install set changed")
    require(package_version("moguet-source-receipt-single") == "1-1", "v1 was not installed")

    status, evidence, causal, installs, _ = run_production_transport(
        "installed-invocation", 1, single_v2[0]
    )
    require(status == "Missing" and evidence == "Missing" and causal == "Absent" and not installs, "v1 -> v2 Upgrade became causal Install")
    require(package_version("moguet-source-receipt-single") == "2-1", "v2 upgrade did not execute")

    status, evidence, causal, installs, _ = run_production_transport(
        "installed-invocation", 2, single_v2[0]
    )
    require(status == "Missing" and evidence == "Missing" and causal == "Absent" and not installs, "same-version reinstall became Install")

    status, evidence, causal, installs, _ = run_production_transport(
        "installed-invocation", 3, single_v1[0]
    )
    require(status == "Missing" and evidence == "Missing" and causal == "Absent" and not installs, "downgrade became Install")
    require(package_version("moguet-source-receipt-single") == "1-1", "downgrade did not execute")

    status, evidence, causal, installs, pacman_status = run_production_transport(
        "installed-invocation", 4, single_v1[0], needed=True
    )
    # No transaction means no trusted hook-phase observation. A numeric zero
    # alone must not manufacture a package-manager outcome or an Install.
    require(status == "OutcomeUnknown" and evidence == "Incomplete" and causal == "Absent" and not installs and pacman_status is None, "--needed skip became Install or a known unobserved outcome")

    # Extend the same actual transaction lifecycle; no new package fixture.
    explicit_before = subprocess.check_output(["/usr/bin/pacman", "-Qqe"])
    subprocess.run(["/usr/bin/pacman", "-R", "--noconfirm", "--", "moguet-source-receipt-single"],
                   check=True, stdout=subprocess.DEVNULL)
    require(package_version("moguet-source-receipt-single") is None, "remove did not remove the fixture")
    status, evidence, causal, installs, pacman_status = run_production_transport(
        "installed-invocation", 6, single_v1[0]
    )
    require(status == "Complete" and evidence == "Complete" and causal == "Established"
            and installs == ["moguet-source-receipt-single"] and pacman_status == 0
            and package_version("moguet-source-receipt-single") == "1-1"
            and subprocess.check_output(["/usr/bin/pacman", "-Qqe"]) == explicit_before,
            "remove/reinstall lost actual Install evidence or install reason")

    status, evidence, causal, installs, pacman_status = run_production_transport(
        "installed-invocation",
        5,
        (0, "moguet-source-receipt-failure", "1-1", "moguet-source-receipt-failure", "any", failure),
    )
    # Missing dependency stops this fixture before PreTransaction hooks.
    require(status == "OutcomeUnknown" and evidence == "Incomplete" and causal == "Absent" and not installs and pacman_status is None, "pre-hook failure became a known pacman result")
    require(package_version("moguet-source-receipt-failure") is None, "failed transaction installed its target")

    replay_token = token("b")
    prepare(replay_token, single_v1)
    replay_state, replay_installs = consume(replay_token)
    require(replay_state == "Missing" and not replay_installs, "empty replay setup produced Install")
    expect_rejection([HELPER, "consume", replay_token], "second consume")
    expect_rejection([HELPER, "abort", replay_token], "abort after consume")
    descriptor = sealed_stream([v1])
    try:
        duplicate = subprocess.run(
            [HELPER, "prepare", replay_token, "moguet-source-receipt-single", "AsDependency", "0", "1", "--", "0", "moguet-source-receipt-single", "1-1", "moguet-source-receipt-single", "any", str(v1.stat().st_size), "0", hashlib.sha256(v1.read_bytes()).hexdigest(), "-"],
            stdin=descriptor,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    finally:
        os.close(descriptor)
    require(duplicate.returncode != 0, "used token was prepared again")

    # Native makepkg emitted an actual sibling dependency; the fixed helper
    # installs both selected archives in one ordinary dependency-checked -U.
    metadata = subprocess.check_output(["/usr/bin/bsdtar", "-xOf", str(multi_a), ".PKGINFO"])
    require(b"\ndepend = moguet-source-receipt-multi-b=1-1\n" in metadata,
            "native split archive lost its sibling dependency")
    multi_state, multi_installs = run_transaction(
        token("2"),
        [
            (1, "moguet-source-receipt-multi-a", "1-1", "moguet-source-receipt-multi", "any", multi_a),
            (4, "moguet-source-receipt-multi-b", "1-1", "moguet-source-receipt-multi", "any", multi_b),
        ],
    )
    require(multi_state == "Complete" and sorted(multi_installs) == ["moguet-source-receipt-multi-a", "moguet-source-receipt-multi-b"], "multi-artifact Install set was not exact")
    require(package_version("moguet-source-receipt-multi-a") == "1-1"
            and package_version("moguet-source-receipt-multi-b") == "1-1",
            "same-base dependency transaction did not install both children")

    expect_rejection([HELPER, "consume", token("3"), SELECTED_OWNER], "source caller-selected owner")
    expect_rejection([HELPER, "consume", "../bad"], "invalid source token")

    selected_token = token("4")
    selected_prepare = subprocess.run(
        [SELECTED_HELPER, "prepare", selected_token, SELECTED_OWNER, "--", "selected-package"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    require(selected_prepare.returncode == 0, "selected-provider isolation setup failed")
    expect_rejection([HELPER, "consume", selected_token], "selected token at source endpoint")
    subprocess.run([SELECTED_HELPER, "abort", selected_token, SELECTED_OWNER], check=True, stdout=subprocess.DEVNULL)

    source_token = token("5")
    prepare(source_token, single_v1)
    expect_rejection([SELECTED_HELPER, "consume", source_token, SELECTED_OWNER], "source token at selected endpoint")
    abort(source_token)

    concurrent_a = token("6")
    concurrent_b = token("7")
    prepare(concurrent_a, single_v1)
    prepare(concurrent_b, single_v1)
    require((STATE_ROOT / "active" / concurrent_a).is_dir() and (STATE_ROOT / "active" / concurrent_b).is_dir(), "concurrent source states collided")
    abort(concurrent_a)
    abort(concurrent_b)

    runtime_mode = stat.S_IMODE(os.stat(STATE_ROOT, follow_symlinks=False).st_mode)
    require(runtime_mode == 0o700, "source state root mode changed")
    print(
        "source-artifact-receipt-installed-validation: cleanup lifecycle Eligible and Install/Upgrade/reinstall/downgrade/needed/failure/multi/isolation checks passed"
    )


if __name__ == "__main__":
    main()
