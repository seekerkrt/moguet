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
    split = False
    mixed = False

    def log_message(self, *args):
        pass

    def do_GET(self):
        requested = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query).get("arg[]", [])
        packages = []
        for name in self.names:
            if requested and name not in requested:
                continue
            base = "example-base" if name == PACKAGE or (self.split and name == PACKAGE + "-tools") or (self.relation == "shared-base" and name == "bootstrap-a") else name
            dependency = "virtual-bootstrap<2" if self.relation == "required-provider" else PACKAGE + "<2"
            depends = [dependency] if self.relation and name == "bootstrap-a" else []
            if self.provider_case and name == "bootstrap-a":
                depends = ["virtual-active"]
            if self.provider_case in ("multi-provider-decline", "multi-provider-failure") and name == PACKAGE:
                depends = ["virtual-declined"]
            provides = ["virtual-bootstrap=1"] if self.relation == "required-provider" and name == PACKAGE else []
            packages.append(dict(Name=name, PackageBase=base,
                                 Version="0.5-1" if self.mixed and name == PACKAGE + "-tools" else self.version if name == PACKAGE or self.decisions_only or (self.split and name == PACKAGE + "-tools") else "2-1",
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
        "no-cache": b"y\ny\n",
        "clean-cache": b"y\ny\n",
        "dirty-pkgbuild": b"y\ny\n",
        "overlay": b"y\ny\n",
        "ignored": b"y\ny\n",
        "wrong-head": b"y\ny\n",
        "malicious-config": b"y\ny\n",
        "supplemental-collision": b"y\ny\n",
        "advance-after-revalidation": b"y\ny\n",
        "acquire-cleanup": b"y\ny\n",
        "s3-failure": b"y\ny\n",
        "acquire-launch": b"y\n",
        "acquire-nonzero": b"y\n",
        "acquire-timeout": b"y\n",
        "acquire-signal": b"y\n",
        "acquire-cancel": b"y\n",
        "acquire-cancel-zero": b"y\n",
        "acquire-metadata": b"y\n",
        "acquire-unavailable": b"y\n",
        "acquire-unsafe": b"y\n",
        "advance-before-revalidation": b"y\n",
        "review-eof": b"y\n\x04",
        "review-decline-cleanup": b"y\nn\n",
        "review-cancel-cleanup": b"y\nq\n",
        "multi-acquire-launch": b"y\n",
        "multi-acquire-cancel-zero": b"y\n",
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
    closure_failure_details = {
        "closure-acquire-nonzero": ("pinned source object unavailable", "Git process exit code: 42"),
        "closure-acquire-timeout": ("Git process failed", "Git process timed out"),
        "closure-acquire-cancel-zero": ("source acquisition cancelled", "Git process exit code: 0", "source acquisition cancellation signal: 2"),
        "closure-acquire-launch": ("Git process failed", "Git process launch/setup failed (errno 2)"),
        "closure-acquire-signal": ("Git process failed", "Git process terminated by signal: 15"),
        "closure-acquire-io": ("Git process failed", "Git process I/O/wait failed (errno 5)"),
        "closure-acquire-limit": ("Git process failed", "Git process output limit exceeded (1024 bytes)"),
        "closure-acquire-cleanup": ("pinned source object unavailable", "Git process exit code: 42", "source acquisition cleanup incomplete; temporary source data may remain"),
    }
    cases |= {case: b"y\ny\n" for case in closure_failure_details}
    # Initial Missing now has a separate exact-closure review after recipe
    # acceptance. Old scenarios retain their original recipe answers.
    for case in cases:
        cases[case] += b"y\n"
    pinned_cases = {name: b"y\ny\ny\n" for name in (
        "pinned-recursive", "pinned-branch", "pinned-generated-output", "pinned-generated-prepared", "pinned-native-output",
        "pinned-generated-extra", "pinned-native-output-extra", "pinned-prepared-root", "pinned-prepared-child",
        "pinned-post-child", "pinned-gitlink", "pinned-declaration", "pinned-missing",
        "pinned-extra", "pinned-cancel", "pinned-build-failure", "pinned-cleanup-refusal")}
    interaction_cases = {
        "multi-pinned-review-q": b"y\ny\nq\n",
        "multi-pinned-review-eof": b"y\ny\n\x04",
        "multi-pinned-review-no": b"y\ny\nn\n",
        "multi-pinned-review-q-cleanup": b"y\ny\nq\n",
    }
    pinned_cases |= interaction_cases
    split_cases = {name: b"y\ny\ny\n" for name in (
        "split-partial", "split-partial-update", "split-both", "split-both-mixed-version", "split-partial-reordered", "split-both-reordered", "split-both-selected-missing", "split-both-nonzero", "split-both-binding-failure", "split-both-publication-failure")}
    split_cases["multi-split-review-cancel"] = b"y\nq\n"
    split_cases["split-both-mixed-decline"] = b"n\n"
    # Bootstrap approval/recipe/closure, then ordinary closure No and Yes.
    split_cases["split-partial-update"] += b"y\n"
    topology_cases = {name: b"y\ny\ny\nn\ny\n" for name in (
        "topology-tree-sitter", "topology-wezterm", "topology-xpadneo")}
    cases |= split_cases | topology_cases
    if len(sys.argv) > 2:
        cases = ({case: cases[case] for case in closure_failure_details} if sys.argv[2] == "--closure-acquisition" else topology_cases if sys.argv[2] == "--topologies" else split_cases if sys.argv[2] == "--split" else interaction_cases if sys.argv[2] == "--closure-interaction" else pinned_cases if sys.argv[2] == "--pinned-s4"
                 else {sys.argv[2]: (cases | pinned_cases | split_cases)[sys.argv[2]]})
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
            Rpc.split = "split-" in case
            Rpc.mixed = "mixed-" in case
            if case.startswith("split-both"):
                Rpc.names.append(PACKAGE + "-tools")
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith("MOGUET_TEST_")}
            env.update(LANG="C", LC_ALL="C", LANGUAGE="", no_proxy="127.0.0.1",
                       NO_PROXY="127.0.0.1",
                       MOGUET_TEST_AUR_RPC_BASE_URL=f"http://127.0.0.1:{server.server_port}/rpc/")
            command = [sys.argv[1], "--devel-bootstrap", case]
            # Representative cases now include an additional refused source
            # review and a complete subsequent update, not only reassessment.
            case_timeout = 180 if case in topology_cases else 90
            if case != "non-tty":
                command = [sys.executable, str(ROOT / "tests/run-with-pty.py"),
                           "--timeout", str(case_timeout), "--", *command]
            completed = subprocess.run(command, input=answer, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, env=env, cwd=ROOT,
                                       timeout=case_timeout + 10, check=False)
            output = completed.stdout.decode("utf-8", errors="replace").replace("\r", "")
            if completed.returncode or f"S553 production {case} PASS" not in output:
                print(output)
                raise SystemExit(f"bootstrap fixture {case} failed: exit {completed.returncode}")
            if case in closure_failure_details:
                for detail in ("source closure acquisition failed during root source acquisition:",
                               "authoritative execution incomplete", *closure_failure_details[case]):
                    if detail not in output:
                        print(output)
                        raise SystemExit(f"closure acquisition CLI detail missing: {case}: {detail}")
                if "closure process output must not be dumped" in output or "Use this exact upstream source snapshot as build input?" in output:
                    raise SystemExit(f"closure acquisition failure leaked output or reached review: {case}")
            if case in topology_cases:
                if output.count("Use this exact upstream source snapshot as build input?") != 3:
                    raise SystemExit(f"representative snapshot acceptance was skipped/repeated: {case}")
                second = output.split("S564 second ordinary begin\n", 1)[1].split("S564 second ordinary end\n", 1)[0]
                if any(word in second for word in ("Warning:", "tracking baseline is missing", "Accept this", "Use this exact", "[y/N]")):
                    raise SystemExit(f"steady-state warning/prompt: {case}: {second}")
                if "The repository system upgrade and normal AUR update completed." not in second:
                    raise SystemExit(f"steady-state normal success presentation missing: {case}")
                if f"S564 topology {case} migration=Complete" not in output:
                    raise SystemExit(f"representative authority-chain oracle missing: {case}")
                for disposition in ("decline", "accept"):
                    begin = f"S564 ordinary changed {disposition} begin\n"
                    end = f"S564 ordinary changed {disposition} end\n"
                    if output.count(begin) != 1 or output.count(end) != 1:
                        raise SystemExit(f"ordinary lifecycle evidence missing: {case}/{disposition}")
                    ordinary = output.split(begin, 1)[1].split(end, 1)[0]
                    if ordinary.count("Use this exact upstream source snapshot as build input?") != 1:
                        raise SystemExit(f"ordinary source acceptance skipped/repeated: {case}/{disposition}")
                    if any(text in ordinary for text in (
                            "tracking baseline is missing", "Accept this full source review for devel tracking bootstrap?")):
                        raise SystemExit(f"ordinary update repeated Missing migration: {case}/{disposition}")
                if f"S564 ordinary {case} decline-preserved=1 update=Complete generation=2 same=UpToDate" not in output:
                    raise SystemExit(f"ordinary update/provenance oracle missing: {case}")
            if case in interaction_cases:
                if f"S564 FG1 {case.removeprefix('multi-')} PASS" not in output or output.count("Use this exact upstream source snapshot as build input?") != 1:
                    raise SystemExit(f"closure interaction oracle missing: {case}")
                expected = "review could not produce explicit acceptance" if case == "multi-pinned-review-no" else "AUR update: Cancelled"
                if expected not in output or "authoritative execution incomplete" in output:
                    print(output)
                    raise SystemExit(f"closure interaction presentation was flattened: {case}")
            if case.startswith("pinned-"):
                if f"S564 4B2 {case} PASS" not in output:
                    raise SystemExit(f"missing closure integration oracle: {case}")
                if output.count("Use this exact upstream source snapshot as build input?") != 1:
                    raise SystemExit(f"closure review was skipped/repeated: {case}")
            if case in ("supplemental", "supplemental-collision"):
                for reviewed_input in ("fix.patch", "config.toml", "reviewed-patch-applied", "reviewed-config"):
                    if reviewed_input not in output:
                        raise SystemExit(f"supplemental full review omitted {reviewed_input}")
            if "S553 lifecycle " in output:
                if output.count("tracking baseline is missing") != 1 or output.count("Accept this full source review for devel tracking bootstrap?") != 1:
                    raise SystemExit(f"migration/review prompt count changed on second ordinary update: {case}")
            if "malicious-old" in output:
                raise SystemExit(f"old cache bytes reached full review: {case}")
            for line in output.splitlines():
                if line.startswith(("S604 ", "S593 ", "S553 lifecycle ", "S564 4B2 ", "S564 FG1 ", "S564 topology ", "S564 ordinary ")):
                    print(line)
            print(f"S553 production {case} PASS")
        server.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
