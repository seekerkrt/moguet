#!/usr/bin/python3
"""Exercise isolation without ever delegating to a host package command."""

import fcntl
import hashlib
import importlib.util
import itertools
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parent.parent
ADAPTER = ROOT / "tests/legacy-install-test-adapter.py"
HELPER = "/usr/local/libexec/moguet/moguet-source-artifact-install-helper"
spec = importlib.util.spec_from_file_location("adapter", ADAPTER)
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


class LegacyInstallAdapterTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.log = self.root / "commands.log"
        self.log.touch()
        self.env = {
            "PATH": str(self.root),  # A PATH trap must never be consulted.
            "MOGUET_TEST_COMMAND_LOG": str(self.log),
            "MOGUET_TEST_SUDO_EXIT_CODE": "0",
            "MOGUET_TEST_SUDO_MAIN_STATUS": "0",
        }
        for name in ("sudo", "pacman", "moguet-source-artifact-install-helper"):
            trap = self.root / name
            trap.write_text(f"#!/bin/sh\necho reached > '{self.root / 'trap'}'\nexit 98\n")
            trap.chmod(0o755)
        self.fd = os.memfd_create("legacy-test", os.MFD_ALLOW_SEALING)
        self.addCleanup(os.close, self.fd)
        self.paths = []
        self.records = []
        for index, name in enumerate(("selected-child", "other-child")):
            path = self.root / f"{name} with space.pkg.tar.zst"
            content = (name + " 1.0-1\narchive bytes").encode()
            signature = (name + " signature").encode()
            path.write_bytes(content)
            Path(str(path) + ".sig").write_bytes(signature)
            self.paths.append(str(path))
            os.write(self.fd, content + signature)
            self.records.extend([
                str(index + 1), name, "1.0-1", "-", "-", str(len(content)), str(len(signature)),
                hashlib.sha256(content).hexdigest(), hashlib.sha256(signature).hexdigest()])
        fcntl.fcntl(self.fd, fcntl.F_ADD_SEALS, adapter.SEALS)
        self.args = ["/usr/bin/sudo", "--", HELPER, "install-legacy", str(os.getpid()), str(self.fd),
                     "a" * 64, "split-base", "PreserveExistingReason", "0", "0", "--", *self.records]

    def run_adapter(self, args=None, paths=None):
        self.log.write_text("")
        result = subprocess.run(
            ["/usr/bin/python3", "-I", str(ADAPTER), HELPER, shlex.join(args or self.args),
             *(self.paths if paths is None else paths)],
            env=self.env, capture_output=True, text=True, check=False)
        self.assertFalse((self.root / "trap").exists())
        return result

    def test_existing_oracles_preserve_flags_selection_order_and_status(self):
        for stub, reason, needed, no_confirm in itertools.product(
                adapter.STUBS, ("PreserveExistingReason", "AsDependency", "AsExplicit"), ("0", "1"), ("0", "1")):
            with self.subTest(stub=stub, reason=reason, needed=needed, no_confirm=no_confirm):
                self.env["MOGUET_TEST_LEGACY_SUDO_STUB"] = str(stub)
                args = self.args.copy()
                args[8:11] = [reason, needed, no_confirm]
                result = self.run_adapter(args)
                self.assertEqual(result.returncode, 0, result.stderr)
                expected = ["sudo", "pacman", "-U"]
                if no_confirm == "1":
                    expected.append("--noconfirm")
                if needed == "1":
                    expected.append("--needed")
                expected.extend({"PreserveExistingReason": [], "AsDependency": ["--asdeps"],
                                 "AsExplicit": ["--asexplicit"]}[reason])
                self.assertEqual(self.log.read_text(), " ".join([*expected, "--", *self.paths]) + "\n")
        self.env["MOGUET_TEST_SUDO_EXIT_CODE"] = "23"
        self.assertEqual(self.run_adapter().returncode, 23)

    def test_missing_or_wrong_hook_is_closed(self):
        for configured in (None, "", "/usr/bin/sudo", HELPER, "/usr/bin/pacman", str(self.root / "sudo")):
            with self.subTest(configured=configured):
                if configured is None:
                    self.env.pop("MOGUET_TEST_LEGACY_SUDO_STUB", None)
                else:
                    self.env["MOGUET_TEST_LEGACY_SUDO_STUB"] = configured
                result = self.run_adapter()
                self.assertEqual(result.returncode, 125, result.stderr)
                self.assertEqual(self.log.read_text(), "")

    def test_displaced_missing_or_nonexecutable_stub_is_closed(self):
        missing = self.root / "missing"
        link = self.root / "redirected"
        link.symlink_to("/usr/bin/sudo")
        nonexecutable = self.root / "nonexecutable"
        nonexecutable.touch(mode=0o600)
        for path in (missing, link, nonexecutable):
            with self.subTest(path=path), patch.object(adapter, "STUBS", (path,)), patch.dict(
                    os.environ, {"MOGUET_TEST_LEGACY_SUDO_STUB": str(path),
                                 "MOGUET_TEST_COMMAND_LOG": str(self.log)}, clear=True):
                with self.assertRaises((OSError, ValueError)):
                    adapter.selected_stub()

    def test_malformed_wire_or_wrong_selection_never_calls_stub(self):
        self.env["MOGUET_TEST_LEGACY_SUDO_STUB"] = str(adapter.STUBS[0])
        for position, value in ((0, "sudo"), (2, "/tmp/helper"), (3, "install"), (4, "-1"),
                                (5, "99999999"), (6, "bad"), (8, "unknown"), (9, "2"),
                                (10, "2"), (11, "bad"), (13, "unselected-child"),
                                (14, "2.0-1"), (19, "0" * 64)):
            with self.subTest(position=position):
                args = self.args.copy()
                args[position] = value
                self.assertEqual(self.run_adapter(args).returncode, 125)
                self.assertEqual(self.log.read_text(), "")
        self.assertEqual(self.run_adapter(paths=self.paths[:1]).returncode, 125)
        self.assertEqual(self.run_adapter(paths=list(reversed(self.paths))).returncode, 125)
        Path(self.paths[0]).write_bytes(b"modified same inode")
        self.assertEqual(self.run_adapter().returncode, 125)
        self.assertEqual(self.log.read_text(), "")

    def test_unsealed_input_never_calls_stub(self):
        self.env["MOGUET_TEST_LEGACY_SUDO_STUB"] = str(adapter.STUBS[0])
        fd = os.memfd_create("unsealed-test", os.MFD_ALLOW_SEALING)
        self.addCleanup(os.close, fd)
        args = self.args.copy()
        args[5] = str(fd)
        self.assertEqual(self.run_adapter(args).returncode, 125)
        self.assertEqual(self.log.read_text(), "")


if __name__ == "__main__":
    unittest.main()
