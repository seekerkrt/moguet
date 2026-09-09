#!/usr/bin/python3
"""S6 actual publication acceptance; the default S5 runner still requires NONE."""

import hashlib
import json
from pathlib import Path
import runpy
import tomllib


s5 = runpy.run_path(str(Path(__file__).with_name("run-exact-installed-binding.py")))
require = s5["require"]
SCHEMA_KEYS = set("""
schema_version source_kind package_base aur_git_remote reviewed_recipe_oid
reviewed_state_generation reviewed_state_document_sha256 evaluated_vcs_kind
evaluated_source_location evaluated_selector_kind evaluated_selector_value
 evaluated_architecture_scope actual_built_git_oid artifact_child
artifact_package_base artifact_full_version artifact_architecture
artifact_archive_sha256 artifact_mtree_sha256 installed_child
installed_package_base installed_full_version installed_architecture
installed_mtree_sha256 installed_database_record_sha256
installed_record_generation_scheme installed_record_generation_identity
""".split())


def validate_publication(output, binding_records):
    rows = [line.split("\t") for line in output.splitlines() if line.startswith("S6C-INSTALLED\t")]
    documents = [line.split("\t") for line in output.splitlines() if line.startswith("S6C-DOCUMENT\t")]
    require(len(rows) == len(documents) == 4, "missing S6 transaction/document rows")
    require(not any(line.startswith("S5C-INSTALLED\t") for line in output.splitlines()), "S6 mode reported publication-none")
    for generation, (row, document, binding) in enumerate(zip(rows, documents, binding_records), 1):
        require(len(row) == 12 and len(document) == 4, "malformed S6 evidence")
        require(row[1:3] == [binding[1], binding[3]], "S6 transaction identity differs from actual receipt/binding")
        require(row[3:8] == ["Succeeded", "Complete", "Complete", "Complete", "Complete"], "S6 independent dimensions incomplete")
        require(row[8] == document[1] == str(generation), "noncontiguous publication order")
        require(row[9] == binding[5], "store/installed generation conflation")
        raw = bytes.fromhex(document[3])
        require(hashlib.sha256(raw).hexdigest() == row[10], "raw file SHA differs from Complete identity")
        model = tomllib.loads(raw.decode("utf-8"))
        require(set(model) == SCHEMA_KEYS and model["schema_version"] == 1, "schema v1/27 keys or live-only exclusion changed")
        require(model["package_base"] == row[11] and model["artifact_full_version"] == model["installed_full_version"] == row[2], "persistent package identity mismatch")
        require(model["installed_record_generation_identity"] == row[9], "persistent installed generation mismatch")
        require(model["artifact_mtree_sha256"] == model["installed_mtree_sha256"] == binding[4], "raw MTREE mismatch")
    require(len({row[9] for row in rows}) == 4, "actual installed generation failed to change on reinstall/downgrade")
    print("devel-publication-container: S4->S5->S6 Install/Upgrade/reinstall/downgrade PASS")
    print("devel-publication-container: raw digest/schema/readback/history 1->2->3->4 PASS")


def main():
    # Record the copied candidate, excluding generated artifacts. Host evidence
    # compares this inventory with the working tree that supplied docker build.
    paths = [Path(name) for name in ("Makefile", "CMakeLists.txt", "CMakePresets.json", ".dockerignore", ".clang-format")]
    for directory in ("source", "tests", "cmake", "scripts", "containers"):
        paths.extend(path for path in Path(directory).rglob("*") if path.is_file() and path.suffix not in (".pyc",) and "__pycache__" not in path.parts and not any(part in ("src", "pkg") for part in path.parts))
    hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(paths)}
    print("S6C-SOURCE-HASHES\t" + json.dumps(hashes, sort_keys=True))
    require({path.name for path in Path("/sys/class/net").iterdir()} == {"lo"}, "network is not isolated")
    s5["main"](validate_publication)


if __name__ == "__main__":
    main()
