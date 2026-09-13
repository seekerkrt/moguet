#!/usr/bin/env python3
"""Isolated production -Syu bootstrap, review, S4/S5/S6 and reassessment."""

import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import urllib.parse

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = "moguet-slice4-bootstrap-git"


class Rpc(http.server.BaseHTTPRequestHandler):
    version = "0-1"
    names = [PACKAGE]
    decisions_only = False
    relation = ""
    provider_case = ""

    def log_message(self, *args):
        pass

    def do_GET(self):
        requested = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query).get("arg[]", [])
        packages = []
        for name in self.names:
            if requested and name not in requested:
                continue
            base = "example-base" if name == PACKAGE or (self.relation == "shared-base" and name == "bootstrap-a") else name
            dependency = "virtual-bootstrap<2" if self.relation == "required-provider" else PACKAGE + "<2"
            depends = [dependency] if self.relation and name == "bootstrap-a" else []
            if self.provider_case and name == "bootstrap-a":
                depends = ["virtual-active"]
            if self.provider_case in ("multi-provider-decline", "multi-provider-failure") and name == PACKAGE:
                depends = ["virtual-declined"]
            provides = ["virtual-bootstrap=1"] if self.relation == "required-provider" and name == PACKAGE else []
            packages.append(dict(Name=name, PackageBase=base,
                                 Version=self.version if name == PACKAGE or self.decisions_only else "2-1",
                                 Description="isolated devel bootstrap fixture", Maintainer="fixture",
                                 Depends=depends, MakeDepends=[], CheckDepends=[], OptDepends=[],
                                 Provides=provides, Conflicts=[], Replaces=[], OutOfDate=None))
        response_type = "multiinfo"
        if "/search/" in self.path:
            response_type = "search"
            packages = [package for package in packages if self.relation == "required-provider" and package["Name"] == PACKAGE]
        body = json.dumps(dict(version=5, type=response_type, resultcount=len(packages), results=packages)).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    cases = {
        "accept": b"y\ny\n",
        "supplemental": b"y\ny\n",
        "older": b"y\ny\n",
        "newer": b"y\n",
        "reviewed-same": b"y\ny\n",
        "reviewed-changed": b"y\ny\n",
        "decline": b"n\n",
        "default-no": b"\n",
        "cancel": b"q\n",
        "eof": b"\x04",
        "non-tty": b"y\ny\n",
        "noconfirm": b"y\ny\n",
        "nodiff": b"y\ny\n",
        "diff-skip": b"y\ny\n",
        "unsupported": b"",
        "invalid": b"",
        "corrupt": b"",
        "future": b"",
        "unsafe": b"",
        "review-decline": b"y\nn\n",
        "review-cancel": b"y\nq\n",
        "build-failure": b"y\ny\n",
        "nonzero": b"y\ny\n",
        "binding-failure": b"y\ny\n",
        "publication-failure": b"y\ny\n",
        "publication-unknown": b"y\ny\n",
        "multi-provider-ordinary": b"1\ny\n",
        "multi-provider-decline": b"1\n1\nn\n",
        "multi-provider-failure": b"1\n1\n\n",
        "local-config-timeout": b"",
        "local-status-timeout": b"",
        "local-config-overflow": b"",
        "local-status-overflow": b"",
        "multi-accept": b"y\ny\n",
        "multi-decline": b"n\n",
        "multi-cancel": b"q\n",
        "multi-review-cancel": b"y\nq\n",
        "multi-build-failure": b"y\ny\n",
        "multi-publication-failure": b"y\ny\n",
        "multi-publication-unknown": b"y\ny\n",
        "multi-all-decline": b"n\nn\nn\n",
        "multi-decision-cancel": b"y\nq\n",
        "multi-decisions": b"y\nn\ny\n",
        "multi-required-dependency": b"",
        "multi-required-provider": b"",
        "multi-shared-base": b"",
    }
    if len(sys.argv) > 2:
        cases = {sys.argv[2]: cases[sys.argv[2]]}
    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Rpc) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        for case, answer in cases.items():
            Rpc.relation = case.removeprefix("multi-") if case in ("multi-required-dependency", "multi-required-provider", "multi-shared-base") else ""
            Rpc.provider_case = case if case.startswith("multi-provider-") else ""
            Rpc.version = "1-1" if Rpc.relation else "2-1" if case in ("newer", "multi-provider-ordinary") else "0-0" if case == "older" else "0-1"
            Rpc.decisions_only = case in ("multi-all-decline", "multi-decision-cancel", "multi-decisions")
            suffix = "-git" if Rpc.decisions_only else ""
            Rpc.names = ["bootstrap-a" + suffix, PACKAGE, "zz-bootstrap-c" + suffix] if case.startswith("multi-") else [PACKAGE]
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith("MOGUET_TEST_")}
            env.update(LANG="C", LC_ALL="C", LANGUAGE="", no_proxy="127.0.0.1",
                       NO_PROXY="127.0.0.1",
                       MOGUET_TEST_AUR_RPC_BASE_URL=f"http://127.0.0.1:{server.server_port}/rpc/")
            command = [sys.argv[1], "--devel-bootstrap", case]
            if case != "non-tty":
                command = [sys.executable, str(ROOT / "tests/run-with-pty.py"),
                           "--timeout", "90", "--", *command]
            completed = subprocess.run(command, input=answer, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, env=env, cwd=ROOT,
                                       timeout=100, check=False)
            output = completed.stdout.decode("utf-8", errors="replace").replace("\r", "")
            if completed.returncode or f"S553 production {case} PASS" not in output:
                print(output)
                raise SystemExit(f"bootstrap fixture {case} failed: exit {completed.returncode}")
            if case.startswith("local-") and "tracking baseline is missing" in output:
                raise SystemExit(f"unavailable local Git observation prompted: {case}")
            if case == "supplemental":
                for reviewed_input in ("fix.patch", "config.toml", "reviewed-patch-applied", "reviewed-config"):
                    if reviewed_input not in output:
                        raise SystemExit(f"supplemental full review omitted {reviewed_input}")
            for line in output.splitlines():
                if line.startswith("S553 lifecycle "):
                    print(line)
            print(f"S553 production {case} PASS")
        server.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
