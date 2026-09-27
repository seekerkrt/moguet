#!/usr/bin/env python3
"""Private fixed-worker transport proof; no shell completion integration."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

helper, worker, build = map(Path, sys.argv[1:])

def expect(condition, message):
    if not condition:
        raise AssertionError(message)

with tempfile.TemporaryDirectory(prefix="moguet-prefix-", dir=os.environ.get("TMPDIR")) as directory:
    root = Path(directory)
    copied = root / helper.name
    shutil.copy2(helper, copied)
    fixed_worker = root / worker.name
    def run(body, prefix="ch"):
        fixed_worker.write_text("#!/usr/bin/python3\n" + body)
        fixed_worker.chmod(0o755)
        started = time.monotonic()
        result = subprocess.run([copied, prefix], capture_output=True, timeout=4)
        return result, time.monotonic() - started

    result, _ = run("print('chrome')\nprint('chromium')\n")
    expect(result.returncode == 0 and result.stdout == b"chrome\nchromium\n" and not result.stderr,
           "direct helper protocol success failed")
    result, _ = run("print('a' * 65535)\n")
    expect(result.returncode == 0 and len(result.stdout) == 65536, "exact output byte boundary rejected")
    result, _ = run("[print(f'pkg{i:04}') for i in range(256)]\n")
    expect(result.returncode == 0 and len(result.stdout.splitlines()) == 256, "exact count boundary rejected")
    result, _ = run("pass\n", "[")
    expect(result.returncode == 0 and not result.stdout, "empty success lost")
    for body in (
        "print('chromium'); raise SystemExit(7)\n",
        "print('bad name')\n", "print('z'); print('a')\n",
        "print('a'); print('a')\n", "print('a', end='')\n",
        "[print(f'pkg{i:04}') for i in range(257)]\n",
        "print('a' * 65536)\n",
    ):
        result, _ = run(body)
        expect(result.returncode == 1 and not result.stdout and result.stderr,
               "failure/invalid/over-budget output leaked to stdout")
    result, elapsed = run("import time\nprint('chromium', flush=True)\ntime.sleep(5)\n")
    expect(result.returncode == 1 and not result.stdout and elapsed < 2,
           "whole worker deadline not enforced")
    print(f"sleeping worker timeout observed: {elapsed:.3f}s (policy 500ms + 50ms grace)")
    # Configuration subprocesses inherit the fixed worker group. A child that
    # ignores SIGTERM must be killed too; stdout activity cannot extend deadline.
    result, elapsed = run("import subprocess, time\nsubprocess.Popen(['/usr/bin/python3', '-c', 'import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(5)'])\ntime.sleep(5)\n")
    expect(result.returncode == 1 and not result.stdout and elapsed < 2, "descendant escaped deadline")
    result, elapsed = run("import time\nwhile True:\n print('a'*1024, flush=True)\n time.sleep(.001)\n")
    expect(result.returncode == 1 and not result.stdout and elapsed < 2, "capture overflow not bounded")
    fixed_worker.unlink()
    result = subprocess.run([copied, "ch"], capture_output=True, timeout=4)
    expect(result.returncode == 1 and not result.stdout, "missing worker became empty success")
    result = subprocess.run([copied, "a" * 257], capture_output=True, timeout=4)
    expect(result.returncode == 2 and not result.stdout, "prefix input limit missing")
    # CMake install component proves both private files share the canonical
    # libexec path; it installs no normal CLI/state/config payload.
    stage = root / "stage"
    install = subprocess.run(["cmake", "--install", str(build), "--component", "repository-prefix"],
                             env={**os.environ, "DESTDIR": str(stage)}, capture_output=True, timeout=10)
    expect(install.returncode == 0, install.stderr.decode())
    installed = sorted(path for path in stage.rglob("*") if path.is_file())
    expect(len(installed) == 2 and {p.name for p in installed} == {helper.name, worker.name} and
           len({p.parent for p in installed}) == 1, "private install closure differs")
    expect(all(p.parent.name == "moguet" and p.parent.parent.name == "libexec" for p in installed),
           "default private libexec installation differs")
    # Broken Moguet config cannot participate in the real helper's startup.
    config = root / "config/moguet"
    config.mkdir(parents=True)
    (config / "config.toml").write_text("[broken TOML !!!")
    state, cache = root / "state", root / "cache"
    result = subprocess.run([helper, "moguet-no-such-prefix["], capture_output=True, timeout=4,
                            env={**os.environ, "XDG_CONFIG_HOME": str(config.parent),
                                 "XDG_STATE_HOME": str(state), "XDG_CACHE_HOME": str(cache)})
    expect(result.returncode == 0 and not result.stdout and not result.stderr,
           "default local provider/broken Moguet config isolation failed")
    expect(not state.exists() and not cache.exists(), "helper created state/cache")
    for prefix in ("ch", "", "ch.*"):
        durations = []
        for _ in range(3):
            started = time.monotonic()
            actual = subprocess.run([helper, prefix], capture_output=True, timeout=4)
            durations.append(time.monotonic() - started)
            expect(actual.returncode == 0 and not actual.stderr, "real local helper invocation failed")
            names = actual.stdout.decode().splitlines()
            expect(len(names) <= 256 and len(actual.stdout) <= 65536 and
                   names == sorted(set(names)) and all(name.startswith(prefix) for name in names),
                   "real helper literal/count/order/protocol mismatch")
        print(f"real helper prefix {prefix!r}: first {durations[0]:.3f}s, repeats {durations[1:]}s, names {len(names)}, bytes {len(actual.stdout)}")
    for executable in (helper, worker):
        symbols = subprocess.check_output(["nm", "-C", str(executable)]).decode()
        for forbidden in ("parse_cli_arguments(", "load_user_config(", "aur_rpc", "prepare_sync_install(", "alpm_trans_init"):
            expect(forbidden not in symbols, "forbidden production dependency: " + forbidden)
    deps = subprocess.check_output(["ldd", str(worker)]).decode()
    expect("libalpm" in deps, "worker not linked to existing libalpm authority")
print("repository prefix helper tests: all checks passed")
