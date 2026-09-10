#!/bin/sh
set -eu

repo_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
. "$repo_root/scripts/validation-status.sh"
tmp_dir=$(mktemp -d)

cleanup() {
    rm -rf "$tmp_dir" >/dev/null 2>&1 || :
}
trap cleanup EXIT

live_root=$repo_root/containers/arch-live-validation
readme_file=$live_root/README.md
local_package_file=$live_root/fixtures/local-package/PKGBUILD
local_contract_file=$live_root/fixtures/local-package/contract.env
local_expected_srcinfo_file=$live_root/fixtures/local-package/expected.srcinfo
local_payload_file=$live_root/fixtures/local-package/payload-authority.tsv
aur_case_file=$live_root/aur-cases.tsv
aur_payload_file=$live_root/fixtures/aur/payload-authority.tsv
aur_case_loader=$live_root/fixtures/aur/load-case.sh
aur_conflict_mutator=$live_root/aur-mutate-pkginfo-conflict.py
live_dockerfile=$live_root/Dockerfile
provider_runner=$live_root/run-provider-selection.sh
pacman_sentinel=$live_root/pacman-sentinel.sh
aur_dockerfile=$live_root/Dockerfile.aur
aur_runner=$live_root/run-aur-install.sh
aur_gateway=$live_root/aur-pacman-gateway.sh
aur_stage_helper=$live_root/aur-stage-artifact.py
aur_metadata_helper=$live_root/aur-archive-metadata-check.c
local_dockerfile=$live_root/Dockerfile.local
local_runner=$live_root/run-local-install.sh
local_gateway=$live_root/local-pacman-gateway.sh
local_stage_helper=$live_root/local-stage-artifact.py
local_archive_validator=$live_root/local-archive-validator.sh
validation_status_library=$repo_root/scripts/validation-status.sh
dockerignore_file=$repo_root/.dockerignore
makefile=$repo_root/Makefile
development_policy=$repo_root/docs/DEVELOPMENT.md
validation_policy=$repo_root/docs/VALIDATION.md
offline_dockerfile=$repo_root/containers/arch-validation/Dockerfile
offline_runner=$repo_root/containers/arch-validation/run-tests.sh
receipt_root=$repo_root/containers/arch-receipt-validation
receipt_dockerfile=$receipt_root/Dockerfile
receipt_runner=$receipt_root/run-installed-receipt.sh
source_receipt_runner=$receipt_root/run-installed-source-artifact-receipt.py
installed_binding_runner=$receipt_root/run-installed-binding-characterization.py
receipt_dependency_v1=$receipt_root/fixtures/dependency-v1/PKGBUILD
receipt_dependency_v2=$receipt_root/fixtures/dependency-v2/PKGBUILD
receipt_target=$receipt_root/fixtures/target/PKGBUILD
receipt_source_v2_different=$receipt_root/fixtures/source-v2-different/PKGBUILD
test_targets_file=$repo_root/cmake/MoguetTestTargets.cmake
production_cmake_file=$repo_root/CMakeLists.txt
artifact_identity_source=$repo_root/source/artifact_identity.cpp
artifact_archive_metadata_source=$repo_root/source/artifact_archive_metadata.cpp
installed_source_fixture=$repo_root/tests/source_artifact_install_installed_fixture.cpp
production_source_runner=$repo_root/source/source_install.cpp

fail() {
    printf '%s\n' "$*" >&2
    exit 1
}

assert_regular_file() {
    path=$1
    label=$2
    if [ ! -f "$path" ] || [ -L "$path" ]; then
        fail "required file must be a regular non-symlink: $label ($path)"
    fi
}

assert_contains() {
    file=$1
    expected=$2
    if ! grep -F -- "$expected" "$file" >/dev/null; then
        fail "missing contract entry in $file: $expected"
    fi
}

assert_not_contains() {
    file=$1
    unexpected=$2
    if grep -F -- "$unexpected" "$file" >/dev/null; then
        fail "forbidden contract entry in $file: $unexpected"
    fi
}

make_target_body() {
    target_name=$1
    awk -v target="$target_name" '
        $0 ~ ("^" target ":[[:space:]]*") {
            in_target = 1
        }
        in_target && $0 !~ ("^" target ":[[:space:]]*") &&
            $0 ~ /^[A-Za-z0-9_.-]+([[:space:]]+[A-Za-z0-9_.-]+)*:/ {
            exit
        }
        in_target {
            print
        }
    ' "$makefile"
}

assert_regular_file "$readme_file" 'live validation contract'
assert_regular_file "$local_package_file" 'local package PKGBUILD'
assert_regular_file "$local_contract_file" 'local package contract env'
assert_regular_file "$local_expected_srcinfo_file" 'local expected .SRCINFO projection'
assert_regular_file "$local_payload_file" 'local payload authority'
assert_regular_file "$aur_case_file" 'AUR case authority table'
assert_regular_file "$aur_payload_file" 'AUR expected payload authority'
assert_regular_file "$aur_case_loader" 'AUR case authority loader'
assert_regular_file "$aur_conflict_mutator" 'AUR conflict mutation helper'
assert_regular_file "$live_dockerfile" 'live provider Dockerfile'
assert_regular_file "$provider_runner" 'live provider runner'
assert_regular_file "$pacman_sentinel" 'live pacman sentinel source'
assert_regular_file "$aur_dockerfile" 'live AUR Dockerfile'
assert_regular_file "$aur_runner" 'live AUR runner'
assert_regular_file "$aur_gateway" 'live AUR pacman gateway'
assert_regular_file "$aur_stage_helper" 'live AUR staging helper'
assert_regular_file "$aur_metadata_helper" 'live AUR libarchive metadata helper'
assert_regular_file "$local_dockerfile" 'live local PKGBUILD Dockerfile'
assert_regular_file "$local_runner" 'live local PKGBUILD runner'
assert_regular_file "$local_gateway" 'live local PKGBUILD gateway'
assert_regular_file "$local_stage_helper" 'live local artifact staging helper'
assert_regular_file "$local_archive_validator" 'live local archive validator'
assert_regular_file "$validation_status_library" 'validation status library'
assert_regular_file "$dockerignore_file" 'Docker context exclusion contract'
assert_regular_file "$development_policy" 'development workflow authority'
assert_regular_file "$validation_policy" 'validation policy authority'
assert_regular_file "$offline_dockerfile" 'offline validation Dockerfile'
assert_regular_file "$offline_runner" 'offline validation runner'
assert_regular_file "$receipt_dockerfile" 'trusted receipt Dockerfile'
assert_regular_file "$receipt_runner" 'trusted receipt installed runner'
assert_regular_file "$source_receipt_runner" \
    'source-artifact receipt installed runner'
assert_regular_file "$installed_binding_runner" \
    'installed-binding characterization runner'
assert_regular_file "$receipt_dependency_v1" 'trusted receipt dependency v1 fixture'
assert_regular_file "$receipt_dependency_v2" 'trusted receipt dependency v2 fixture'
assert_regular_file "$receipt_target" 'trusted receipt solver target fixture'
assert_regular_file "$receipt_source_v2_different" \
    'same-version different-artifact fixture'
assert_regular_file "$artifact_identity_source" 'production artifact identity owner'
assert_regular_file "$artifact_archive_metadata_source" \
    'production artifact archive metadata owner'

assert_contains "$readme_file" "live laneは既存のoffline validation lane"
assert_contains "$readme_file" "ベースイメージは \`archlinux:latest\` を利用する"
assert_contains "$readme_file" "実行時ネットワークは**有効**"
assert_contains "$readme_file" "--privileged"
assert_contains "$readme_file" "暗黙のフォールバックを許容せず"
assert_contains "$readme_file" "make test\` / \`release-check\` を再帰的に起動しない"
# Values shared by the local scenario are read from its small identity contract.
# Their independent PKGBUILD/.SRCINFO/payload parity is checked by
# test-fixture-authority, rather than copied here as another expected-data set.
# shellcheck source=../containers/arch-live-validation/fixtures/local-package/contract.env
. "$local_contract_file"
local_pkgver=${PACKAGE_VERSION%-*}
local_pkgrel=${PACKAGE_VERSION##*-}
assert_contains "$local_package_file" "pkgname='$PACKAGE_NAME'"
assert_contains "$local_package_file" "pkgver=$local_pkgver"
assert_contains "$local_package_file" "pkgrel=$local_pkgrel"
assert_contains "$local_package_file" "makedepends=('$REQUIRED_MAKE_DEPENDENCY')"
if grep -E '^source=\(.+\)$' "$local_package_file" >/dev/null; then
    fail 'local fixture source list must be empty for no source download'
fi
if ! grep -F 'source=()' "$local_package_file" >/dev/null; then
    fail 'local fixture must not require external source'
fi
if ! grep -F 'live-fixture-marker' "$local_package_file" >/dev/null; then
    fail 'local fixture package() must install marker artifact'
fi
if ! grep -F '"${CC:-cc}" -g -O0' "$local_package_file" >/dev/null; then
    fail 'local fixture must generate debug symbols for the live debug artifact'
fi
if ! grep -F "usr/bin/$PACKAGE_NAME" "$local_package_file" >/dev/null; then
    fail 'local fixture package() must install its debug-symbol source binary'
fi
if [ -e "$live_root/fixtures/local-package/.SRCINFO" ]; then
    fail 'local fixture must not track .SRCINFO for live contract'
fi

if grep -F 'SELECTED_MAKE_PROVIDER_EXPECTED=' "$local_contract_file" >/dev/null; then
    fail 'local fixture must not duplicate selected provider identity'
fi

# AUR authority format and payload identity are checked independently by
# test-fixture-authority.  This static gate loads that one scenario only to
# prove every live transport consumes the tracked values instead of copies.
# shellcheck source=../containers/arch-live-validation/fixtures/aur/load-case.sh
. "$aur_case_loader"
validation_load_aur_case "$aur_case_file" || fail 'AUR case authority did not load'
for pinned_aur_value in \
    "$AUR_CASE_EXPECTED_VERSION" \
    "$AUR_CASE_EXPECTED_GIT_HEAD" \
    "$AUR_CASE_EXPECTED_PKGBUILD_SHA256" \
    "$AUR_CASE_EXPECTED_SRCINFO_SHA256" \
    "$AUR_CASE_EXPECTED_SOURCE_FILENAME" \
    "$AUR_CASE_EXPECTED_SOURCE_URL" \
    "$AUR_CASE_EXPECTED_SOURCE_SHA256" \
    "$AUR_CASE_EXPECTED_RPC_URL_PATH"
do
    for aur_consumer in \
        "$aur_dockerfile" "$aur_gateway" "$aur_runner" "$aur_stage_helper"
    do
        if grep -F -- "$pinned_aur_value" "$aur_consumer" >/dev/null; then
            fail "AUR consumer duplicates pinned scenario value: $aur_consumer"
        else
            authority_grep_status=$?
        fi
        case $authority_grep_status in
            1) ;;
            *) fail "AUR consumer scan failed with status $authority_grep_status: $aur_consumer" ;;
        esac
    done
done

# The conflict-policy negative artifact is scenario-derived, not an
# independent dependency oracle.  Extract and execute the runner's actual
# mutator argument construction so a temporary authority change crosses the
# loader -> runner -> mutator boundary.
validate_runner_conflict_mutation_contract() {
    checked_runner=$1
    extracted_function=$2
    python3 - "$checked_runner" "$extracted_function" <<'PY'
from pathlib import Path
import re
import sys

runner_path, extracted_path = map(Path, sys.argv[1:])
runner = runner_path.read_text(encoding="utf-8")
function_start = "mutate_conflict_policy_archive() {\n"
if runner.count(function_start) != 1:
    raise SystemExit("conflict mutation runner function is not unique")
start_index = runner.index(function_start)
end_index = runner.find("\n}\n", start_index)
if end_index < 0:
    raise SystemExit("conflict mutation runner function is not closed")
function_text = runner[start_index:end_index + 3]
expected_function = (
    "mutate_conflict_policy_archive() {\n"
    "    mutation_archive_path=$1\n"
    '    python3 "$conflict_mutator" \\\n'
    '        "$mutation_archive_path" "$AUR_CASE_MAKE_DEPENDENCIES"\n'
    "}\n"
)
if function_text != expected_function:
    raise SystemExit(
        "conflict mutation runner does not pass loaded AUR make dependencies directly"
    )
if re.search(r"(?m)^[ \t]*make_dependencies=", runner):
    raise SystemExit("runner reintroduces a make-dependency alias assignment")
if '"$make_dependencies"' in runner:
    raise SystemExit("runner consumes a make-dependency alias")
if re.search(
    r"(?<![A-Za-z0-9_])AUR_CASE_MAKE_DEPENDENCIES"
    r"[ \t]*(?:\+?=|:=)",
    runner,
):
    raise SystemExit("runner reassigns loaded AUR make-dependency authority")
start_marker = "current_phase=conflict-policy-negative"
end_marker = "/usr/bin/zstd --quiet --force -o \"$conflict_gateway_artifact\""
if runner.count(start_marker) != 1 or runner.count(end_marker) != 1:
    raise SystemExit("conflict mutation block markers are not unique")
block = runner.split(start_marker, 1)[1].split(end_marker, 1)[0]
expected_call = 'if mutate_conflict_policy_archive "$conflict_raw_tar"; then'
if block.count(expected_call) != 1:
    raise SystemExit("conflict mutation block does not invoke runner construction once")
if re.search(r"makedepend\s*=", block):
    raise SystemExit("conflict mutation block contains a consumer-side dependency record")
extracted_path.write_text(function_text, encoding="utf-8")
PY
}

runner_mutation_function=$tmp_dir/runner-conflict-mutation.sh
if validate_runner_conflict_mutation_contract \
    "$aur_runner" "$runner_mutation_function"; then
    :
else
    runner_contract_status=$?
    fail "AUR conflict mutation runner contract failed with status $runner_contract_status"
fi

# Focused regression for the previous counterexample: a consumer-side literal
# assignment must be rejected even if the direct mutator call remains intact.
literal_runner=$tmp_dir/run-aur-install-literal.sh
python3 - "$aur_runner" "$literal_runner" <<'PY' || fail 'literal runner counterexample setup failed'
from pathlib import Path
import sys

source, destination = map(Path, sys.argv[1:])
runner = source.read_text(encoding="utf-8")
anchor = "runtime_dependencies=$AUR_CASE_RUNTIME_DEPENDENCIES\n"
if runner.count(anchor) != 1:
    raise SystemExit("runner make-dependency authority anchor is not unique")
destination.write_text(
    runner.replace(anchor, anchor + "make_dependencies=fixture-build-literal\n", 1),
    encoding="utf-8",
)
PY
if validation_expect_status \
    aur-runner-literal-counterexample 1 \
    "$tmp_dir/aur-runner-literal.stdout" \
    "$tmp_dir/aur-runner-literal.stderr" \
    validate_runner_conflict_mutation_contract \
    "$literal_runner" "$tmp_dir/literal-runner-function.sh"; then
    :
else
    fail 'consumer-side make-dependency literal counterexample was not rejected'
fi
printf '%s\n' '  consumer-side make-dependency literal counterexample: rejected'

# The scenario loader is the only setter for the uppercase authority field.
# Reject a detached consumer alias that shadows the loaded value before the
# otherwise-direct runner function reaches the mutator.
shadow_runner=$tmp_dir/run-aur-install-uppercase-shadow.sh
python3 - "$aur_runner" "$shadow_runner" <<'PY' || fail 'uppercase-shadow runner counterexample setup failed'
from pathlib import Path
import sys

source, destination = map(Path, sys.argv[1:])
runner = source.read_text(encoding="utf-8")
anchor = "runtime_dependencies=$AUR_CASE_RUNTIME_DEPENDENCIES\n"
if runner.count(anchor) != 1:
    raise SystemExit("runner uppercase-shadow authority anchor is not unique")
destination.write_text(
    runner.replace(
        anchor,
        anchor
        + "detached_make_dependencies=fixture-build-literal\n"
        + "AUR_CASE_MAKE_DEPENDENCIES=$detached_make_dependencies\n",
        1,
    ),
    encoding="utf-8",
)
PY
if validation_expect_status \
    aur-runner-uppercase-shadow-counterexample 1 \
    "$tmp_dir/aur-runner-uppercase-shadow.stdout" \
    "$tmp_dir/aur-runner-uppercase-shadow.stderr" \
    validate_runner_conflict_mutation_contract \
    "$shadow_runner" "$tmp_dir/uppercase-shadow-runner-function.sh"; then
    :
else
    fail 'uppercase-shadow make-dependency counterexample was not rejected'
fi
printf '%s\n' '  uppercase-shadow make-dependency counterexample: rejected'

aur_mutation_case=$tmp_dir/aur-mutation-case.tsv
expected_aur_mutation_make_dependencies='fixture-build-one>=1.0,fixture-build-two'
python3 - \
    "$aur_case_file" "$aur_mutation_case" \
    "$expected_aur_mutation_make_dependencies" <<'PY' || fail 'temporary AUR dependency authority setup failed'
import csv
from pathlib import Path
import sys

source = Path(sys.argv[1])
destination = Path(sys.argv[2])
expected_make_dependencies = sys.argv[3]
with source.open(encoding="utf-8", newline="") as stream:
    rows = list(csv.reader(stream, delimiter="\t"))
if len(rows) != 2 or "make_dependencies" not in rows[0]:
    raise SystemExit("tracked AUR scenario shape drift")
make_dependencies_index = rows[0].index("make_dependencies")
rows[1][make_dependencies_index] = expected_make_dependencies
with destination.open("w", encoding="utf-8", newline="") as stream:
    writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
    writer.writerows(rows)
PY
validation_load_aur_case "$aur_mutation_case" ||
    fail 'temporary AUR dependency authority did not load'
[ "$AUR_CASE_MAKE_DEPENDENCIES" = \
    "$expected_aur_mutation_make_dependencies" ] ||
    fail 'temporary AUR make-dependency authority did not load exactly'
printf '  changed AUR authority exact value: %s\n' \
    "$AUR_CASE_MAKE_DEPENDENCIES"

aur_mutation_positive=$tmp_dir/aur-mutation-positive.tar
aur_mutation_zero=$tmp_dir/aur-mutation-zero.tar
aur_mutation_multiple=$tmp_dir/aur-mutation-multiple.tar
python3 - \
    "$aur_mutation_positive" "$aur_mutation_zero" "$aur_mutation_multiple" \
    "$AUR_CASE_MAKE_DEPENDENCIES" <<'PY' || fail 'temporary AUR PKGINFO mutation archives could not be created'
from io import BytesIO
from pathlib import Path
import sys
import tarfile

positive_path, zero_path, multiple_path = map(Path, sys.argv[1:4])
dependencies = sys.argv[4].split(",")
if len(dependencies) < 2 or any(not dependency for dependency in dependencies):
    raise SystemExit("temporary make dependency authority is not a useful separator case")


def write_archive(path: Path, records: list[str]) -> None:
    contents = "pkgname = fixture\n" + "".join(
        f"makedepend = {record}\n" for record in records
    )
    payload = contents.encode("ascii")
    member = tarfile.TarInfo(".PKGINFO")
    member.mode = 0o644
    member.size = len(payload)
    with tarfile.open(path, mode="w") as archive:
        archive.addfile(member, BytesIO(payload))


write_archive(positive_path, dependencies)
write_archive(zero_path, dependencies[1:])
write_archive(multiple_path, [dependencies[0], dependencies[0], *dependencies[1:]])
PY

# shellcheck source=/dev/null
. "$runner_mutation_function"
conflict_mutator=$aur_conflict_mutator
if mutate_conflict_policy_archive "$aur_mutation_positive"; then
    :
else
    mutation_status=$?
    fail "scenario-derived AUR conflict mutation failed with status $mutation_status"
fi
python3 - "$aur_mutation_positive" "$AUR_CASE_MAKE_DEPENDENCIES" <<'PY' || fail 'scenario-derived AUR conflict mutation result is invalid'
from pathlib import Path
import sys
import tarfile

archive_path = Path(sys.argv[1])
dependencies = sys.argv[2].split(",")
with tarfile.open(archive_path, mode="r:") as archive:
    members = [member for member in archive.getmembers() if member.name == ".PKGINFO"]
    if len(members) != 1:
        raise SystemExit("mutated archive has no unique .PKGINFO")
    extracted = archive.extractfile(members[0])
    if extracted is None:
        raise SystemExit("mutated .PKGINFO is not readable")
    records = extracted.read().decode("ascii").splitlines()

target_dependency = dependencies[0]
if f"makedepend = {target_dependency}" in records:
    raise SystemExit("scenario-selected make dependency was not mutated")
replacement = f"conflict = {'x' * (len(target_dependency) + 2)}"
if records.count(replacement) != 1:
    raise SystemExit("derived conflict record is not exact and unique")
for dependency in dependencies[1:]:
    if records.count(f"makedepend = {dependency}") != 1:
        raise SystemExit("non-target scenario make dependency changed")
PY
printf '%s\n' '  changed AUR authority reached the runner mutator argument'

for mutation_case in zero multiple
do
    case $mutation_case in
        zero) mutation_archive=$aur_mutation_zero ;;
        multiple) mutation_archive=$aur_mutation_multiple ;;
        *) fail "unknown AUR conflict mutation regression: $mutation_case" ;;
    esac
    if validation_expect_status \
        "aur-conflict-mutation-$mutation_case" 1 \
        "$tmp_dir/aur-conflict-mutation-$mutation_case.stdout" \
        "$tmp_dir/aur-conflict-mutation-$mutation_case.stderr" \
        python3 "$aur_conflict_mutator" \
        "$mutation_archive" "$AUR_CASE_MAKE_DEPENDENCIES"; then
        :
    else
        fail "AUR conflict mutation $mutation_case target did not fail closed"
    fi
done
if validation_expect_status \
    aur-conflict-mutation-empty-authority 1 \
    "$tmp_dir/aur-conflict-mutation-empty.stdout" \
    "$tmp_dir/aur-conflict-mutation-empty.stderr" \
    python3 "$aur_conflict_mutator" "$aur_mutation_zero" ''; then
    :
else
    fail 'AUR conflict mutation empty authority did not fail closed'
fi

# Restore the tracked case for the static checks below.
validation_load_aur_case "$aur_case_file" ||
    fail 'tracked AUR case authority did not reload after mutation regression'

python3 - \
    "$aur_payload_file" "$local_payload_file" \
    "$aur_dockerfile" "$aur_gateway" "$aur_runner" "$aur_stage_helper" \
    "$local_dockerfile" "$local_gateway" "$local_runner" \
    "$local_archive_validator" <<'PY' || fail 'payload hash authority is duplicated by a consumer'
from pathlib import Path
import sys

aur_payload, local_payload = map(Path, sys.argv[1:3])
aur_consumers = [Path(value) for value in sys.argv[3:7]]
local_consumers = [Path(value) for value in sys.argv[7:11]]

def pinned_hashes(path):
    rows = [line.split("\t") for line in path.read_text(encoding="utf-8").splitlines()[1:]]
    return {row[3] for row in rows if len(row) == 4 and row[3] != "-"}

for authority, consumers in (
    (aur_payload, aur_consumers),
    (local_payload, local_consumers),
):
    for consumer in consumers:
        text = consumer.read_text(encoding="utf-8")
        for digest in pinned_hashes(authority):
            if digest in text:
                raise SystemExit(f"{consumer} duplicates payload digest {digest}")
PY

# Ensure offline validation lane is not forced as a live dependency by contract text.
assert_contains "$readme_file" "offline validation lane"

# Slice 2 keeps a standalone image and production binary/runtime route.
assert_contains "$live_dockerfile" 'FROM archlinux:latest'
assert_contains "$live_dockerfile" 'pacman -Syu --needed --noconfirm'
assert_contains "$live_dockerfile" 'COPY --chown=moguet-validation:moguet-validation . .'
assert_contains "$live_dockerfile" 'USER moguet-validation:moguet-validation'
assert_contains "$live_dockerfile" 'make -j8 --output-sync=target'
assert_contains "$live_dockerfile" 'CMD ["containers/arch-live-validation/run-provider-selection.sh"]'
assert_contains "$provider_runner" 'production_moguet=$repo_root/moguet'
assert_contains "$provider_runner" 'makepkg --printsrcinfo > .SRCINFO'
assert_contains "$provider_runner" 'cmp -s "$fixture_expected_srcinfo" "$case_source/.SRCINFO"'
assert_contains "$provider_runner" 'python3 "$pty_runner" --timeout 90 --'
assert_contains "$provider_runner" 'candidate_package'
assert_contains "$provider_runner" 'first_provider_choice=$(awk'
assert_contains "$provider_runner" 'second_provider_choice=$(awk'
assert_contains "$provider_runner" 'case ",$EXPECTED_PROVIDER_PACKAGES," in'

old_ifs=$IFS
IFS=,
set -- $EXPECTED_PROVIDER_PACKAGES
IFS=$old_ifs
for forbidden_provider_package in "$@" "$REQUIRED_MAKE_DEPENDENCY"
do
    if grep -E "^[[:space:]]+$forbidden_provider_package([[:space:]\\\\]|$)" \
        "$live_dockerfile" >/dev/null; then
        fail "live image preinstalls fixture provider: $forbidden_provider_package"
    else
        provider_grep_status=$?
    fi
    case $provider_grep_status in
        1) ;;
        *) fail "live image provider scan failed with status $provider_grep_status" ;;
    esac
done
if grep -E '(^|[[:space:]])(include|inherit|COPY)[[:space:]].*arch-validation' \
    "$live_dockerfile" >/dev/null; then
    fail 'live Dockerfile must not include, inherit, or copy the offline lane'
fi
assert_not_contains "$live_dockerfile" 'containers/arch-validation'

# Host metadata, credentials, state, and artifacts never enter COPY layers.
for excluded_path in \
    '.git' \
    '.agents' \
    '.codex' \
    '.ssh' \
    '.gnupg' \
    '.git-credentials' \
    '.netrc' \
    '*.sock' \
    '**/__pycache__' \
    '**/*.py[cod]'
do
    assert_contains "$dockerignore_file" "$excluded_path"
done

# The canonical pacman path is replaced only inside the fully provisioned
# image.  Both the source and runtime copy are immutable to the validation
# user, and the original executable is root-only and never exec'd by a case.
assert_contains "$live_dockerfile" 'install -o root -g root -m 0555'
assert_contains "$live_dockerfile" 'pacman-sentinel.sh'
assert_contains "$live_dockerfile" '/usr/bin/pacman'
assert_contains "$live_dockerfile" '/usr/libexec/moguet-live-validation/pacman.real'
assert_contains "$live_dockerfile" '/usr/libexec/moguet-live-validation/contract.env'
assert_contains "$live_dockerfile" 'install -d -o root -g root -m 0711'
assert_contains "$live_dockerfile" 'moguet-validation ALL=(root) NOPASSWD: /usr/bin/pacman *'
assert_contains "$pacman_sentinel" 'readonly_sentinel_status=86'
assert_contains "$pacman_sentinel" "printf '%s\\0' sudo pacman"
assert_contains "$pacman_sentinel" "[ \"\$#\" -eq 5 ]"
assert_contains "$pacman_sentinel" "[ \"\$#\" -eq 6 ]"
assert_contains "$pacman_sentinel" "[ \"\$1\" = '-S' ]"
assert_contains "$pacman_sentinel" "[ \"\$2\" = '--asdeps' ]"
assert_contains "$pacman_sentinel" "[ \"\$3\" = '--needed' ]"
assert_contains "$pacman_sentinel" "[ \"\$4\" = '--noconfirm' ]"
assert_contains "$pacman_sentinel" '. "$contract_file"'
assert_contains "$pacman_sentinel" 'first_provider_target=$EXPECTED_PROVIDER_REPOSITORY/$first_provider'
assert_contains "$pacman_sentinel" 'second_provider_target=$EXPECTED_PROVIDER_REPOSITORY/$second_provider'
assert_contains "$pacman_sentinel" "reject 'target is not an allowed repo-qualified provider'"
assert_contains "$provider_runner" "sentinel-reject-syu 'rejected argv'"
assert_not_contains "$pacman_sentinel" 'pacman.real'
assert_not_contains "$pacman_sentinel" 'exec '

# Candidate numbers are derived from identity fields rather than fixed order.
if grep -E 'provider_choice=[12]($|[^0-9])' "$provider_runner" >/dev/null; then
    fail 'provider runner must not hard-code reviewed candidate numbers'
fi
assert_contains "$provider_runner" "candidate_source\" != repository"
assert_contains "$provider_runner" 'candidate_repository" != "$EXPECTED_PROVIDER_REPOSITORY"'
assert_contains "$provider_runner" 'candidate_dependency" != "$REQUIRED_MAKE_DEPENDENCY"'
assert_contains "$provider_runner" 'provider candidate set contains duplicate package identities'
assert_contains "$provider_runner" 'provider presentation changed after discovery'
assert_contains "$provider_runner" 'unexpected provider $candidate_package'
assert_not_contains "$provider_runner" 'fallback_package='
assert_not_contains "$provider_runner" 'fallback_dependency='

# Slice 3 is a standalone AUR image with a separate root gateway and runner.
assert_contains "$aur_dockerfile" 'FROM archlinux:latest'
assert_contains "$aur_dockerfile" 'pacman -Syu --needed --noconfirm'
for required_aur_image_package in \
    acl base-devel curl git libarchive python sudo util-linux zstd
do
    assert_contains "$aur_dockerfile" "$required_aur_image_package"
done
assert_contains "$aur_dockerfile" 'validation_load_aur_case'
assert_contains "$aur_dockerfile" 'test "$(uname -m)" = "$AUR_CASE_EXPECTED_ARCHITECTURE"'
assert_contains "$aur_dockerfile" 'pacman -Qq "$AUR_CASE_PACKAGE_NAME"'
assert_contains "$aur_dockerfile" \
    'NoExtract = !$AUR_CASE_README_PATH'
assert_contains "$aur_dockerfile" \
    'grep -Fx -- "!$AUR_CASE_README_PATH"'
assert_contains "$aur_dockerfile" \
    'COPY --chown=moguet-validation:moguet-validation . .'
assert_contains "$aur_dockerfile" 'USER moguet-validation:moguet-validation'
assert_contains "$aur_dockerfile" 'make -j8 --output-sync=target'
assert_contains "$aur_dockerfile" '/usr/libexec/moguet-live-aur/pacman.real'
assert_contains "$aur_dockerfile" 'aur-pacman-gateway.sh'
assert_contains "$aur_dockerfile" '/usr/bin/pacman'
assert_contains "$aur_dockerfile" 'aur-cases.tsv'
assert_contains "$aur_dockerfile" 'fixtures/aur/payload-authority.tsv'
assert_contains "$aur_dockerfile" 'aur-archive-metadata-check.c'
assert_contains "$aur_dockerfile" '-std=c11 -Wall -Wextra -Wpedantic -Werror'
assert_contains "$aur_dockerfile" '-larchive'
assert_contains "$aur_dockerfile" '/usr/libexec/moguet-live-aur/aur-archive-metadata-check'
assert_contains "$aur_dockerfile" 'chmod 0555'
assert_contains "$aur_dockerfile" 'install -o root -g root -m 0444'
assert_contains "$aur_dockerfile" '/usr/libexec/moguet-live-aur/fixtures/reference-payload.tsv'
assert_contains "$aur_dockerfile" '/usr/libexec/moguet-live-aur/fixtures/reference-pkginfo.tsv'
assert_contains "$aur_dockerfile" 'pkginfo-manifest'
assert_contains "$aur_dockerfile" 'reference-build'
assert_contains "$aur_dockerfile" 'git clone --no-checkout'
assert_contains "$aur_dockerfile" '"$AUR_CASE_EXPECTED_GIT_HEAD"'
assert_contains "$aur_dockerfile" '"$AUR_CASE_EXPECTED_PKGBUILD_SHA256"'
assert_contains "$aur_dockerfile" '"$AUR_CASE_EXPECTED_SRCINFO_SHA256"'
assert_contains "$aur_dockerfile" '"$AUR_CASE_EXPECTED_SOURCE_SHA256"'
assert_contains "$aur_dockerfile" '/usr/bin/makepkg --config'
assert_contains "$aur_dockerfile" '--nodeps --noconfirm'
assert_contains "$aur_dockerfile" '/usr/bin/python3 -I /usr/libexec/moguet-live-aur/aur-stage-artifact.py'
assert_contains "$aur_dockerfile" 'manifest'
assert_contains "$aur_dockerfile" 'rm -rf -- "$reference_root"'
assert_contains "$aur_dockerfile" \
    'install -d -o root -g root -m 0755'
assert_contains "$aur_dockerfile" \
    'install -d -o root -g moguet-validation -m 0750'
assert_contains "$aur_dockerfile" \
    'moguet-validation ALL=(root) NOPASSWD: /usr/bin/pacman *'
assert_contains "$aur_dockerfile" \
    'Defaults:moguet-validation env_keep += "MOGUET_LIVE_AUR_CASE"'
assert_contains "$aur_dockerfile" \
    'visudo -cf /etc/sudoers.d/moguet-live-aur'
assert_contains "$aur_dockerfile" \
    'CMD ["containers/arch-live-validation/run-aur-install.sh"]'
assert_not_contains "$aur_dockerfile" \
    'NOPASSWD: /usr/libexec/moguet-live-aur/pacman.real'
assert_not_contains "$aur_dockerfile" 'pacman-sentinel.sh'
assert_not_contains "$aur_dockerfile" 'run-provider-selection.sh'
assert_not_contains "$aur_dockerfile" 'containers/arch-validation'
if grep -E '(^|[[:space:]])(include|inherit|COPY)[[:space:]].*Dockerfile' \
    "$aur_dockerfile" >/dev/null; then
    fail 'AUR Dockerfile must not include, inherit, or copy another Dockerfile'
fi

assert_contains "$aur_gateway" '"/proc/$$/status"'
assert_contains "$aur_gateway" 'if [ "$kernel_effective_uid" -ne 0 ]'
assert_contains "$aur_gateway" 'PATH=/usr/bin'
assert_contains "$aur_gateway" 'export PATH'
assert_contains "$aur_gateway" 'unset PYTHONPATH'
assert_contains "$aur_gateway" 'unset PYTHONHOME'
assert_contains "$aur_gateway" 'exec /usr/bin/env -i PATH=/usr/bin LC_ALL=C'
assert_contains "$aur_gateway" 'validation_load_aur_case "$case_policy"'
assert_contains "$aur_gateway" 'aur-content-drift-test'
assert_contains "$aur_gateway" 'aur-conflict-policy-test'
assert_contains "$aur_gateway" 'aur-xattr-metadata-test'
assert_contains "$aur_gateway" 'aur-acl-metadata-test'
assert_contains "$aur_gateway" 'aur-pkgdesc-authority-test'
assert_contains "$aur_gateway" \
    'missing or unknown MOGUET_LIVE_AUR_CASE'
assert_contains "$aur_gateway" '[ "$#" -ne 4 ]'
assert_contains "$aur_gateway" '[ "$1" != -U ]'
assert_contains "$aur_gateway" '[ "$2" != --noconfirm ]'
assert_contains "$aur_gateway" '[ "$3" != -- ]'
assert_contains "$aur_gateway" \
    'root argv must be exactly: -U --noconfirm -- <one artifact>'
assert_contains "$aur_gateway" \
    '/home/moguet-validation/.cache/moguet/.artifact-workspace~-*/*'
assert_contains "$aur_gateway" 'raw artifact path differs from its canonical realpath'
assert_contains "$aur_gateway" 'source artifact is group/other writable'
assert_contains "$aur_gateway" 'source artifact has an unexpected hard-link count'
assert_contains "$aur_gateway" 'mkdir -m 0750 -- "$evidence_directory"'
assert_contains "$aur_gateway" "printf '%s\\0' sudo pacman"
assert_contains "$aur_gateway" 'stage-hashes.txt'
assert_contains "$aur_gateway" 'archive-metadata-check.txt'
assert_contains "$aur_gateway" '/usr/bin/python3 -I "$stage_helper" stage'
assert_contains "$aur_gateway" 'metadata_helper=/usr/libexec/moguet-live-aur/aur-archive-metadata-check'
assert_contains "$aur_gateway" 'staged artifact direct metadata validation failed'
assert_contains "$aur_gateway" '/usr/bin/python3 -I "$stage_helper" validate'
assert_contains "$aur_gateway" 'reference-pkginfo.tsv'
assert_contains "$aur_gateway" 'staged artifact path, content, or PKGINFO validation failed'
assert_contains "$aur_gateway" 'negative test case must never invoke real pacman'
assert_contains "$aur_gateway" 'gateway_reject_status=97'
assert_contains "$aur_gateway" \
    'write_evidence_line "$evidence_directory/package-identity.txt"'
assert_contains "$aur_gateway" "'transaction=exactly-once'"
assert_contains "$aur_gateway" \
    'write_evidence_line "$evidence_directory/real-pacman-exec.txt"'
assert_contains "$aur_gateway" \
    'exec_real_pacman -U --noconfirm -- "$staged_artifact"'
metadata_check_line=$(grep -n -F '"$metadata_helper" "$staged_artifact"' \
    "$aur_gateway" | cut -d: -f1)
pkginfo_validation_line=$(grep -n -F \
    '/usr/bin/python3 -I "$stage_helper" validate' "$aur_gateway" | cut -d: -f1)
[ -n "$metadata_check_line" ] && [ -n "$pkginfo_validation_line" ] ||
    fail 'gateway must invoke the metadata helper and staged package validator'
[ "$metadata_check_line" -lt "$pkginfo_validation_line" ] ||
    fail 'gateway must inspect direct archive metadata before bsdtar-based validation'
assert_not_contains "$aur_gateway" 'pacman -S '
assert_not_contains "$aur_gateway" 'pacman -R '
assert_not_contains "$aur_gateway" 'pacman -Syu'

assert_contains "$aur_stage_helper" 'os.O_NOFOLLOW'
assert_contains "$aur_stage_helper" 'os.O_EXCL'
assert_contains "$aur_stage_helper" 'source.parent.parent != CACHE_ROOT'
assert_contains "$aur_stage_helper" 'source artifact is not owned by the validation user'
assert_contains "$aur_stage_helper" 'source artifact is group/other writable'
assert_contains "$aur_stage_helper" 'source artifact has an unexpected hard-link count'
assert_contains "$aur_stage_helper" 'source_hash_before'
assert_contains "$aur_stage_helper" 'copied_hash'
assert_contains "$aur_stage_helper" 'staged_hash'
assert_contains "$aur_stage_helper" 'source_hash_after'
assert_contains "$aur_stage_helper" 'len({source_hash_before, copied_hash, staged_hash, source_hash_after})'
assert_contains "$aur_stage_helper" 'def parse_scenario('
assert_contains "$aur_stage_helper" 'required_regular = {'
assert_contains "$aur_stage_helper" 'PKGINFO_FORBIDDEN_TRANSACTION_FIELDS'
assert_contains "$aur_stage_helper" 'PKGINFO_ALLOWED_KEYS'
assert_contains "$aur_stage_helper" 'PKGINFO_STABLE_KEYS'
assert_contains "$aur_stage_helper" 'PKGINFO_MANIFEST_HEADER'
assert_contains "$aur_stage_helper" 'Counter(reference_pairs)'
assert_contains "$aur_stage_helper" '.PKGINFO {key} authority mismatch'
assert_contains "$aur_stage_helper" 'builddate must be one non-negative ASCII decimal integer'
assert_contains "$aur_stage_helper" 'packager has unsafe whitespace or control characters'
assert_contains "$aur_stage_helper" 'archive member owner/group is not root/root'
assert_contains "$aur_stage_helper" '["/usr/bin/bsdtar", *arguments]'
assert_contains "$aur_stage_helper" 'archive payload content hash drift'

assert_contains "$aur_metadata_helper" 'archive_entry_acl_types'
assert_contains "$aur_metadata_helper" 'archive_entry_xattr_count'
assert_contains "$aur_metadata_helper" 'archive_entry_fflags'
assert_contains "$aur_metadata_helper" 'reject_entry("acl"'
assert_contains "$aur_metadata_helper" 'reject_entry("xattr"'
assert_contains "$aur_metadata_helper" 'reject_entry("fflags"'
assert_contains "$aur_metadata_helper" 'category=archive-read'

# Normal production owns archive identity through libalpm.  Test-hook builds
# may retain a process stub, but the live runner must not infer production
# authority from that private implementation detail.
assert_contains "$artifact_identity_source" \
    'artifact_archive_metadata::query_with_libalpm('
assert_contains "$artifact_archive_metadata_source" 'alpm_pkg_load('
assert_contains "$artifact_archive_metadata_source" \
    'independent from pacman output/localization parsing'
assert_not_contains "$artifact_identity_source" 'Logger::raw_cmd'

for gateway_rejection_shape in \
    'run_gateway_rejection syu "$gateway_case" -Syu' \
    'run_gateway_rejection remove "$gateway_case" -R "$package_name"' \
    '-U -- /tmp/fake.pkg.tar.zst' \
    '-U --noconfirm -- /tmp/path1.pkg.tar.zst /tmp/path2.pkg.tar.zst' \
    '-U --asdeps -- /tmp/fake.pkg.tar.zst' \
    'run_gateway_rejection unknown-case unknown-case -Syu' \
    'run_gateway_rejection missing-case missing -Syu'
do
    assert_contains "$aur_runner" "$gateway_rejection_shape"
done
assert_contains "$aur_runner" '"$production_moguet" -Si --aur "$package_name"'
assert_contains "$aur_runner" 'production_input=$case_root/production-install.input'
assert_contains "$aur_runner" "printf 'n\\n' >\"\$production_input\""
assert_contains "$aur_runner" '<"$production_input" >"$production_output"'
assert_not_contains "$aur_runner" "printf 'n\\n' | env MOGUET_LIVE_AUR_CASE"
assert_contains "$aur_runner" \
    '"$production_moguet" --nodiff --noconfirm -S --aur "$package_name"'
if grep -E '"\$production_moguet".*--noedit' "$aur_runner" >/dev/null; then
    fail 'production AUR invocation must not use --noedit'
fi
assert_contains "$aur_runner" 'Review target: PKGBUILD'
assert_contains "$aur_runner" 'tests/run-with-pty.py'
assert_contains "$aur_runner" 'validation_load_aur_case "$case_policy"'
assert_contains "$aur_runner" 'git ls-remote "$aur_git_url" HEAD'
assert_contains "$aur_runner" 'git clone --depth 1 --single-branch --no-checkout'
assert_contains "$aur_runner" "'makepkg' '--packagelist'"
assert_contains "$aur_runner" "'makepkg' '-sc' '--noconfirm'"
assert_contains "$aur_runner" 'PackageBase result: $package_base'
assert_contains "$aur_runner" \
    'produced artifact: $AUR_CASE_DEBUG_PACKAGE_NAME $expected_version (not selected; not installed)'
assert_not_contains "$aur_runner" \
    "Running: LC_ALL=C 'pacman' '-Qp' '--color' 'never' '--'"
assert_not_contains "$aur_runner" 'artifact_identity_query_prefix='
assert_contains "$aur_runner" \
    'gateway argv artifact does not match original artifact evidence'
assert_contains "$aur_runner" 'gateway package identity evidence drift'
assert_contains "$aur_runner" 'gateway validation-complete evidence drift'
assert_contains "$aur_runner" 'gateway real-pacman execution evidence drift'
assert_contains "$aur_runner" \
    'moguet-live-aur-archive-metadata: accepted: entries='
assert_contains "$aur_runner" \
    'package_query=$(LC_ALL=C pacman -Qp --color never -- \'
assert_contains "$aur_runner" 'production did not clean the original artifact workspace'
assert_contains "$aur_runner" 'root gateway binary-content fail-closed test'
assert_contains "$aur_runner" 'binary_payload_path=usr/bin/$package_name'
assert_contains "$aur_runner" 'content-drift gateway rejection changed package inventory'
assert_contains "$aur_runner" 'content-drift rejection consumed valid one-shot evidence'
assert_contains "$aur_runner" 'root gateway transaction-metadata fail-closed test'
assert_contains "$aur_runner" 'forbidden transaction field: conflict'
assert_contains "$aur_runner" 'conflict-policy gateway rejection changed package inventory'
assert_contains "$aur_runner" 'root gateway xattr metadata fail-closed test'
assert_contains "$aur_runner" 'user.moguet-live-test'
assert_contains "$aur_runner" '--xattrs --acls'
assert_contains "$aur_runner" '"category=xattr entry=$binary_payload_path"'
assert_contains "$aur_runner" 'root gateway ACL metadata fail-closed test'
assert_contains "$aur_runner" 'setfacl -m u:65534:rx'
assert_contains "$aur_runner" '"category=acl entry=$binary_payload_path"'
assert_contains "$aur_runner" 'root gateway PKGINFO authority fail-closed test'
assert_contains "$aur_runner" '.PKGINFO pkgdesc authority mismatch'
assert_contains "$aur_runner" 'assert_repacked_path_set'
assert_contains "$aur_runner" 'assert_independent_artifact_unchanged'
assert_contains "$aur_runner" 'negative artifact was retained near the positive install target'
assert_contains "$aur_runner" 'one positive and five independent negative evidence directories'
assert_contains "$aur_runner" 'real pacman not reached'
assert_contains "$aur_runner" 'pacman -Qe "$package_name"'
assert_contains "$aur_runner" 'pacman -Qd "$package_name"'
assert_contains "$aur_runner" 'pacman -Q "$AUR_CASE_DEBUG_PACKAGE_NAME"'
assert_contains "$aur_runner" 'dependency version or install reason changed'
assert_contains "$aur_runner" \
    'container pacman config does not retain the exact expected README'
assert_not_contains "$aur_runner" 'fallback_package='
assert_not_contains "$aur_runner" 'fallback_dependency='

# Provider and offline sources never acquire the AUR real-install gateway.
for protected_lane_file in \
    "$live_dockerfile" "$provider_runner" "$pacman_sentinel" \
    "$offline_dockerfile" "$offline_runner"
do
    assert_not_contains "$protected_lane_file" 'moguet-live-aur'
    assert_not_contains "$protected_lane_file" 'aur-pacman-gateway'
done

# The explicit live target has no host mount, privileged mode, Docker socket,
# host package state, or runtime network disablement.  Failure remains the
# docker command's non-zero status.
live_target=$(make_target_body test-container-live-provider)
printf '%s\n' "$live_target" | grep -F -- '$(DOCKER) build --pull' >/dev/null ||
    fail 'live target must use docker build --pull'
printf '%s\n' "$live_target" | grep -F -- \
    '--file containers/arch-live-validation/Dockerfile' >/dev/null ||
    fail 'live target must select the standalone live Dockerfile'
printf '%s\n' "$live_target" | grep -F -- '$(DOCKER) run --rm' >/dev/null ||
    fail 'live target must destroy its container with --rm'
for forbidden_runtime_option in \
    '--privileged' \
    '--network=none' \
    '--mount' \
    '--volume' \
    'docker.sock' \
    '/var/lib/pacman' \
    '/etc/pacman.conf' \
    '/var/cache/pacman'
do
    if printf '%s\n' "$live_target" | grep -F -- "$forbidden_runtime_option" >/dev/null; then
        fail "live Make target contains forbidden runtime option: $forbidden_runtime_option"
    fi
done

live_target_reference_count=$(validation_grep_count -F -c \
    'test-container-live-provider' "$makefile")
if [ "$live_target_reference_count" -ne 3 ]; then
    fail 'live provider target must appear only in .PHONY, its definition, and the aggregate gate'
fi

aur_live_target=$(make_target_body test-container-live-aur)
printf '%s\n' "$aur_live_target" | grep -F -- \
    '$(DOCKER) build --pull' >/dev/null ||
    fail 'live AUR target must use docker build --pull'
printf '%s\n' "$aur_live_target" | grep -F -- \
    '--file containers/arch-live-validation/Dockerfile.aur' >/dev/null ||
    fail 'live AUR target must select the standalone AUR Dockerfile'
printf '%s\n' "$aur_live_target" | grep -F -- \
    '$(DOCKER) run --rm' >/dev/null ||
    fail 'live AUR target must destroy its container with --rm'
for forbidden_runtime_option in \
    '--privileged' \
    '--network=none' \
    '--mount' \
    '--volume' \
    'docker.sock' \
    '/var/lib/pacman' \
    '/etc/pacman.conf' \
    '/var/cache/pacman'
do
    if printf '%s\n' "$aur_live_target" |
        grep -F -- "$forbidden_runtime_option" >/dev/null; then
        fail "live AUR Make target contains forbidden runtime option: $forbidden_runtime_option"
    fi
done
aur_live_target_reference_count=$(validation_grep_count -F -c \
    'test-container-live-aur' "$makefile")
if [ "$aur_live_target_reference_count" -ne 3 ]; then
    fail 'live AUR target must appear only in .PHONY, its definition, and the aggregate gate'
fi

local_live_target=$(make_target_body test-container-live-local)
printf '%s\n' "$local_live_target" | grep -F -- \
    '$(DOCKER) build --pull' >/dev/null ||
    fail 'live local target must use docker build --pull'
printf '%s\n' "$local_live_target" | grep -F -- \
    '--file containers/arch-live-validation/Dockerfile.local' >/dev/null ||
    fail 'live local target must select the standalone local Dockerfile'
printf '%s\n' "$local_live_target" | grep -F -- \
    '$(DOCKER) run --rm' >/dev/null ||
    fail 'live local target must destroy its container with --rm'
for forbidden_runtime_option in \
    '--privileged' \
    '--network=none' \
    '--mount' \
    '--volume' \
    'docker.sock' \
    '/var/lib/pacman' \
    '/etc/pacman.conf' \
    '/var/cache/pacman'
do
    if printf '%s\n' "$local_live_target" |
        grep -F -- "$forbidden_runtime_option" >/dev/null; then
        fail "live local Make target contains forbidden runtime option: $forbidden_runtime_option"
    fi
done
local_live_target_reference_count=$(validation_grep_count -F -c \
    'test-container-live-local' "$makefile")
if [ "$local_live_target_reference_count" -ne 3 ]; then
    fail 'live local target must appear only in .PHONY, its definition, and the aggregate gate'
fi
aggregate_live_target=$(make_target_body test-container-live)
printf '%s\n' "$aggregate_live_target" | grep -Fx \
    'test-container-live:' >/dev/null ||
    fail 'live aggregate gate must be an explicit target'
printf '%s\n' "$aggregate_live_target" | grep -F \
    '+@set -eu;' >/dev/null ||
    fail 'live aggregate gate must use one fail-fast recursive recipe'
printf '%s\n' "$aggregate_live_target" | awk '
    index($0, "$(MAKE) test-container-live-provider") {
        if (stage != 0) exit 1
        stage = 1
        next
    }
    index($0, "$(MAKE) test-container-live-aur") {
        if (stage != 1) exit 1
        stage = 2
        next
    }
    index($0, "$(MAKE) test-container-live-local") {
        if (stage != 2) exit 1
        stage = 3
        next
    }
    END { exit stage == 3 ? 0 : 1 }
' || fail 'live aggregate gate must run provider, AUR, and local in exact order'

# Exercise the real aggregate under parallel make with a harmless Docker
# double. A provider failure must prevent both later recursive lane calls.
aggregate_probe_root=$(mktemp -d)
trap 'rm -rf -- "$aggregate_probe_root"' EXIT HUP INT TERM
aggregate_docker_probe=$aggregate_probe_root/docker-probe.sh
aggregate_log=$aggregate_probe_root/aggregate.log
printf '%s\n' \
    '#!/bin/sh' \
    'set -eu' \
    'printf "%s\n" "$*" >> "$MOGUET_LIVE_AGGREGATE_LOG"' \
    'if [ -n "${MOGUET_LIVE_AGGREGATE_FAIL_IMAGE-}" ]; then' \
    '    for argument in "$@"; do' \
    '        if [ "$argument" = "$MOGUET_LIVE_AGGREGATE_FAIL_IMAGE" ]; then' \
    '            exit 41' \
    '        fi' \
    '    done' \
    'fi' \
    > "$aggregate_docker_probe"
chmod 0755 "$aggregate_docker_probe"
MOGUET_LIVE_AGGREGATE_LOG=$aggregate_log \
    env -u MAKEFLAGS -u MFLAGS \
    make -j8 --no-print-directory -f "$makefile" \
        DOCKER="$aggregate_docker_probe" test-container-live >/dev/null
awk '
    BEGIN {
        expected[1] = "moguet-arch-live-validation:local"
        expected[2] = "moguet-arch-live-validation:local"
        expected[3] = "moguet-arch-live-aur-validation:local"
        expected[4] = "moguet-arch-live-aur-validation:local"
        expected[5] = "moguet-arch-live-local-validation:local"
        expected[6] = "moguet-arch-live-local-validation:local"
    }
    {
        found = ""
        for (field = 1; field <= NF; ++field) {
            if ($field == expected[1] || $field == expected[3] ||
                $field == expected[5]) {
                found = $field
            }
        }
        if (found != expected[NR]) exit 1
    }
    END { exit NR == 6 ? 0 : 1 }
' "$aggregate_log" ||
    fail 'parallel live aggregate did not preserve provider, AUR, local order'

: > "$aggregate_log"
if MOGUET_LIVE_AGGREGATE_LOG=$aggregate_log \
    MOGUET_LIVE_AGGREGATE_FAIL_IMAGE=moguet-arch-live-validation:local \
    env -u MAKEFLAGS -u MFLAGS \
    make -j8 --no-print-directory -f "$makefile" \
        DOCKER="$aggregate_docker_probe" test-container-live >/dev/null 2>&1; then
    fail 'parallel live aggregate ignored a provider-lane failure'
fi
[ "$(wc -l < "$aggregate_log")" -eq 1 ] &&
    grep -F 'moguet-arch-live-validation:local' "$aggregate_log" >/dev/null &&
    ! grep -E 'moguet-arch-live-(aur|local)-validation:local' \
        "$aggregate_log" >/dev/null ||
    fail 'parallel live aggregate started a later lane after provider failure'

# The local lane owns its root gateway and staging helper. Its broad system
# transaction shapes remain rejected, while AUR/provider/offline lanes do not
# inherit this authority.
assert_contains "$local_dockerfile" 'FROM archlinux:latest'
assert_contains "$local_dockerfile" '. /tmp/moguet-live-local-contract.env'
assert_contains "$local_dockerfile" 'for provider_package in $EXPECTED_PROVIDER_PACKAGES'
assert_contains "$local_dockerfile" 'pacman -Qq "$PACKAGE_NAME"'
assert_contains "$local_dockerfile" 'COPY --chown=moguet-validation:moguet-validation . .'
assert_contains "$local_dockerfile" '/usr/libexec/moguet-live-local/fixtures/local-package'
assert_contains "$local_dockerfile" 'install -o root -g root -m 0444'
awk '
    index($0, "containers/arch-live-validation/fixtures/local-package/PKGBUILD") {
        authority_line = NR
    }
    index($0, "RUN env -u MAKEFLAGS -u MFLAGS make clean") {
        build_line = NR
    }
    END {
        exit authority_line > 0 && build_line > authority_line ? 0 : 1
    }
' "$local_dockerfile" ||
    fail 'root-owned fixture authority must be fixed before validation-user build'
assert_contains "$local_dockerfile" '/usr/libexec/moguet-live-local/pacman.real'
assert_contains "$local_dockerfile" 'local-pacman-gateway.sh'
assert_contains "$local_dockerfile" 'local-stage-artifact.py'
assert_contains "$local_dockerfile" 'local-archive-validator.sh'
assert_contains "$local_dockerfile" 'scripts/validation-status.sh'
assert_contains "$local_dockerfile" 'moguet-validation ALL=(root) NOPASSWD: /usr/bin/pacman *'
assert_contains "$local_dockerfile" 'CMD ["sh", "containers/arch-live-validation/run-local-install.sh"]'
assert_not_contains "$local_dockerfile" 'aur-pacman-gateway.sh'
assert_not_contains "$local_dockerfile" 'pacman-sentinel.sh'
assert_contains "$local_gateway" '"/proc/$$/status"'
assert_contains "$local_gateway" 'PATH=/usr/bin'
assert_contains "$local_gateway" 'unset PYTHONPATH'
assert_contains "$local_gateway" 'case ",$EXPECTED_PROVIDER_PACKAGES," in'
assert_contains "$local_gateway" 'exec_real_pacman --noconfirm "$@"'
assert_contains "$local_gateway" 'exec_real_pacman --noconfirm -U --asexplicit -- "$staged_artifact"'
assert_contains "$local_gateway" 'root argv must be one selected provider transaction or local artifact install'
assert_contains "$local_gateway" 'artifact path is outside the invocation-owned cache prefix'
assert_contains "$local_gateway" 'live-local-case/actual/cache/moguet/.artifact-workspace~-*/*'
assert_contains "$local_gateway" 'local-stage-artifact.py'
assert_contains "$local_gateway" 'local-archive-validator.sh'
assert_contains "$local_gateway" 'validation-status.sh'
assert_contains "$local_gateway" 'archive validator failed with infrastructure status'
assert_not_contains "$local_gateway" 'pacman -Syu'
assert_not_contains "$local_gateway" 'pacman -R '
assert_contains "$local_archive_validator" 'validation_capture_sorted_output'
assert_contains "$local_archive_validator" 'archive-members.raw'
assert_contains "$local_archive_validator" 'expected-members.raw'
assert_contains "$local_archive_validator" 'artifact member listing failed with status'
assert_contains "$local_archive_validator" 'artifact payload path set drift'
assert_contains "$local_archive_validator" 'moguet-live-local-gateway: rejected:'
if grep -E 'bsdtar[^|]*\|[^|]*sort' \
    "$local_gateway" "$local_archive_validator" >/dev/null; then
    fail 'local archive inspection must not pipe bsdtar directly into sort'
fi
awk '
    index($0, "\"$archive_validator\" \"$staged_artifact\"") {
        validator_line = NR
    }
    index($0, "accepted.argv") {
        accepted_line = NR
    }
    index($0, "exec_real_pacman --noconfirm -U --asexplicit") {
        pacman_line = NR
    }
    END {
        exit validator_line > 0 && accepted_line > validator_line &&
            pacman_line > accepted_line ? 0 : 1
    }
' "$local_gateway" ||
    fail 'archive validation must finish before accepted or real-pacman evidence'
assert_contains "$local_stage_helper" 'os.O_NOFOLLOW'
assert_contains "$local_stage_helper" 'os.O_EXCL'
assert_contains "$local_stage_helper" 'live-local-case/actual/cache/moguet'
assert_contains "$local_stage_helper" 'source artifact is not owned by the validation user'
assert_contains "$local_stage_helper" 'source and staged artifact content hashes differ'
assert_contains "$local_stage_helper" 'source_before='
assert_contains "$local_stage_helper" 'source_after='
assert_contains "$local_runner" '--noedit build --local'
assert_contains "$local_runner" 'fixture_root=/usr/libexec/moguet-live-local/fixtures/local-package'
assert_contains "$local_runner" 'fixture_before_copy=$(fixture_manifest)'
assert_contains "$local_runner" 'fixture_after_copy=$(fixture_manifest)'
assert_contains "$local_runner" 'case_copy_manifest=$(source_content_manifest "$prepared_root/source")'
assert_contains "$local_runner" 'chmod 0644 "$prepared_root/source/PKGBUILD"'
assert_contains "$local_runner" 'case-local source copy differs from the root-owned fixture authority'
assert_contains "$local_runner" 'validation user can modify the root-owned local fixture authority'
assert_contains "$local_runner" 'ambiguous providers: $REQUIRED_MAKE_DEPENDENCY'
assert_contains "$local_runner" 'reviewed provider choice'
assert_contains "$local_runner" 'root did not install as the exact explicit fixture package'
assert_contains "$local_runner" 'selected provider did not retain dependency install reason'
assert_contains "$local_runner" 'unselected local debug artifact was installed'
assert_contains "$local_runner" 'baseline package version or reason changed'
assert_contains "$local_runner" 'gateway rejection self-test changed package inventory'
assert_contains "$local_runner" "Running: 'sudo' 'pacman' '-U' '--'"
assert_contains "$local_runner" \
    'required child: $fixture_name $fixture_version (explicit): installed'

for protected_lane_file in \
    "$live_dockerfile" "$provider_runner" "$pacman_sentinel" \
    "$aur_dockerfile" "$aur_runner" "$aur_gateway" "$aur_stage_helper" \
    "$offline_dockerfile" "$offline_runner"
do
    assert_not_contains "$protected_lane_file" 'moguet-live-local'
    assert_not_contains "$protected_lane_file" 'local-pacman-gateway'
done

test_target=$(make_target_body test)
release_target=$(make_target_body release-check)
host_release_target=$(make_target_body test-host-release)
release_exclusive_target=$(make_target_body release-check-exclusive)
if printf '%s\n%s\n' "$test_target" "$release_target" |
    grep -E 'test-container-live-(provider|aur|local)' >/dev/null; then
    fail 'make test or release-check must not recursively start the live lane'
fi
printf '%s\n' "$release_target" | grep -F 'test-live-contract' >/dev/null ||
    fail 'release-check must run the static live-contract gate'
printf '%s\n' "$release_target" | grep -F 'test-fixture-authority' >/dev/null ||
    fail 'release-check must run the fixture-authority gate'
printf '%s\n' "$host_release_target" | grep -F 'test-host-release: test' >/dev/null ||
    fail 'host release composite must own the full host test prerequisite'
printf '%s\n' "$host_release_target" |
    grep -F 'release-check-exclusive' >/dev/null ||
    fail 'host release composite must run the release-exclusive owner'
printf '%s\n' "$release_target" | grep -F 'release-check-exclusive' >/dev/null ||
    fail 'release-check must preserve G coverage through the exclusive owner'
for release_checker in \
    scripts/check-release-version.sh \
    scripts/check-license-compliance.sh \
    scripts/check-packaging-metadata.sh \
    scripts/check-markdown-links.sh
do
    printf '%s\n' "$release_exclusive_target" |
        grep -F "$release_checker" >/dev/null ||
        fail "release-exclusive owner lost checker: $release_checker"
done

# The policy owner must remain discoverable from the development workflow and
# preserve the same named boundaries enforced above. These assertions do not
# duplicate the matrix; they prevent documentation from silently promoting a
# focused or compatibility target to approval authority.
assert_contains "$development_policy" '[VALIDATION.md](VALIDATION.md)'
assert_contains "$validation_policy" 'PR / mergeのcanonical host gateは`test-host-release`である。'
assert_contains "$validation_policy" '`release-check`はstandalone互換targetとして維持するが、full host A–Dのapproval evidenceではない。'
assert_contains "$validation_policy" '`test-container`はhost A–D / Gを代替せず、`test-live-contract`はactual Fを代替しない。'
assert_contains "$validation_policy" 'focused / affected resultだけはPR approval tokenにはならない。'
assert_contains "$validation_policy" 'release candidateは新しいapproval evidence epochである。'
assert_contains "$validation_policy" '## Evidence reuse / invalidation'
assert_contains "$validation_policy" '## Review stop rule / closure rule'

# Existing offline files and target remain isolated from the new live paths.
assert_not_contains "$offline_dockerfile" 'arch-live-validation'
assert_not_contains "$offline_runner" 'arch-live-validation'
assert_contains "$offline_dockerfile" 'RUN env -u MAKEFLAGS -u MFLAGS make clean'
assert_contains "$offline_dockerfile" \
    'RUN env -u MAKEFLAGS -u MFLAGS make -j8 --output-sync=target'
assert_contains "$offline_runner" 'image clean production build artifact is missing'
assert_contains "$offline_runner" 'test-host-release'
assert_not_contains "$offline_runner" 'make clean'
assert_not_contains "$offline_runner" 'parallel-build'
assert_not_contains "$offline_runner" 'parallel-release-check'
for cmake_consumer in \
    "$offline_dockerfile" "$live_dockerfile" "$aur_dockerfile" "$local_dockerfile"
do
    assert_contains "$cmake_consumer" '        cmake \'
done
offline_target=$(make_target_body test-container)
case "$offline_target" in
    *arch-live-validation*)
        fail 'existing test-container recipe contains live-lane paths'
        ;;
esac
printf '%s\n' "$offline_target" | grep -F -- '--network=none' >/dev/null ||
    fail 'existing offline target lost its offline runtime network boundary'

# Slice 3.6 uses an installed root-owned helper in a standalone ephemeral
# container. It never substitutes a root-executed development-tree binary for
# package provenance and both image build and runtime are networkless.
assert_contains "$receipt_dockerfile" 'FROM moguet-arch-validation:local'
assert_contains "$receipt_dockerfile" 'COPY --chown=moguet-validation:moguet-validation . .'
assert_contains "$receipt_dockerfile" 'PREFIX=/usr -j8 --output-sync=target'
assert_contains "$receipt_dockerfile" 'RUN cmake --install build/cmake-production'
assert_contains "$receipt_dockerfile" '/usr/libexec/moguet/moguet-alpm-receipt-helper'
assert_contains "$receipt_dockerfile" \
    '/usr/libexec/moguet/moguet-source-artifact-install-helper'
assert_contains "$receipt_dockerfile" \
    'moguet-source-artifact-install-installed-fixture'
assert_contains "$receipt_dockerfile" \
    '/etc/sudoers.d/moguet-source-artifact-receipt'
assert_contains "$receipt_runner" 'root:root:755:regular file'
assert_contains "$receipt_runner" '--hookdir "$install_hook_directory"'
assert_contains "$receipt_runner" 'INSTALL'
assert_contains "$receipt_runner" 'STATE'
assert_contains "$receipt_runner" 'solver-introduced dependency'
assert_contains "$receipt_runner" 'Upgrade transaction produced an Install receipt'
assert_contains "$receipt_runner" 'failed transaction published a Complete receipt'
receipt_target_body=$(make_target_body test-container-receipt)
printf '%s\n' "$receipt_target_body" | grep -F -- '--network=none' >/dev/null ||
    fail 'trusted receipt target lost its network-none boundary'
printf '%s\n' "$receipt_target_body" | grep -F -- '--file containers/arch-receipt-validation/Dockerfile' >/dev/null ||
    fail 'trusted receipt target does not build its standalone installed fixture'
assert_contains "$source_receipt_runner" 'os.MFD_ALLOW_SEALING'
assert_contains "$source_receipt_runner" 'F_SEAL_WRITE'
assert_contains "$source_receipt_runner" 'moguet-source-artifact-install-helper'
assert_contains "$source_receipt_runner" 'TRANSPORT_FIXTURE'
assert_contains "$source_receipt_runner" 'run_production_transport'
assert_contains "$source_receipt_runner" 'run_cleanup_lifecycle'
assert_contains "$source_receipt_runner" 'run_cleanup_authority_scenario'
assert_contains "$source_receipt_runner" 'later-failed'
assert_contains "$source_receipt_runner" 'later-not-attempted'
assert_contains "$source_receipt_runner" 'CAUSAL'
assert_contains "$source_receipt_runner" 'same-version reinstall became Install'
assert_contains "$source_receipt_runner" 'downgrade became Install'
assert_contains "$source_receipt_runner" '--needed skip became Install'
assert_contains "$source_receipt_runner" 'multi-artifact Install set was not exact'
source_receipt_target_body=$(make_target_body test-container-source-artifact-receipt)
printf '%s\n' "$source_receipt_target_body" | grep -F -- '--network=none' >/dev/null ||
    fail 'source-artifact receipt target lost its network-none boundary'
printf '%s\n' "$source_receipt_target_body" | grep -F -- \
    'run-installed-source-artifact-receipt.py' >/dev/null ||
    fail 'source-artifact receipt target does not run its owner-specific fixture'
cleanup_authority_target_body=$(make_target_body test-container-cleanup-authority)
printf '%s\n' "$cleanup_authority_target_body" | grep -F -- '--network=none' >/dev/null ||
    fail 'cleanup-authority target lost its network-none boundary'
printf '%s\n' "$cleanup_authority_target_body" | grep -F -- \
    'run-installed-source-artifact-receipt.py' >/dev/null ||
    fail 'cleanup-authority target does not run its installed lifecycle fixture'
printf '%s\n' "$cleanup_authority_target_body" | grep -F -- \
    'later-failed later-not-attempted' >/dev/null ||
    fail 'cleanup-authority target lost its production-runner scenario matrix'
printf '%s\n' "$cleanup_authority_target_body" | grep -F -- \
    'positive and installed transport matrix' >/dev/null ||
    fail 'cleanup-authority target lost its existing installed transport matrix'
assert_contains "$receipt_dockerfile" 'receipt-packages-alternate'
assert_contains "$receipt_dockerfile" 'fixtures/source-v2-different'
assert_contains "$installed_binding_runner" 'name_to_handle_at'
assert_contains "$installed_binding_runner" '--probe-record'
assert_contains "$installed_binding_runner" 'same-version identical same-second reinstall'
assert_contains "$installed_binding_runner" 'UnsupportedGeneration'
assert_contains "$installed_binding_runner" 'installed binding matrix:'
installed_binding_target_body=$(make_target_body test-container-installed-binding-characterization)
printf '%s\n' "$installed_binding_target_body" | grep -F -- '--network=none' >/dev/null ||
    fail 'installed-binding characterization lost its network-none boundary'
printf '%s\n' "$installed_binding_target_body" | grep -F -- \
    '--mount type=volume,destination=/var/lib/moguet-installed-binding-characterization,volume-nocopy' >/dev/null ||
    fail 'installed-binding characterization lost its isolated volume'
printf '%s\n' "$installed_binding_target_body" | grep -F -- \
    'run-installed-binding-characterization.py' >/dev/null ||
    fail 'installed-binding characterization does not run its fixture'
if printf '%s\n' "$installed_binding_target_body" | grep -F -- \
    '/var/lib/pacman' >/dev/null; then
    fail 'installed-binding characterization references the host package DB'
fi
exact_binding_runner=$repo_root/containers/arch-receipt-validation/run-exact-installed-binding.py
assert_contains "$exact_binding_runner" 'STATE_ROOT.is_mount()'
assert_contains "$exact_binding_runner" 'Path("/.dockerenv").is_file()'
assert_contains "$exact_binding_runner" 'same-version-reinstall'
assert_contains "$exact_binding_runner" 'downgrade'
assert_contains "$exact_binding_runner" 'host package DB unavailable'
assert_contains "$exact_binding_runner" 'S5C-INSTALLED'
assert_contains "$exact_binding_runner" 'cleanup-Complete'
assert_contains "$exact_binding_runner" 'publication-none'
assert_contains "$test_targets_file" 'MOGUET_ENABLE_DEVEL_SOURCE_ARTIFACT_INSTALL_TEST_HOOKS'
assert_not_contains "$production_cmake_file" 'MOGUET_ENABLE_DEVEL_SOURCE_ARTIFACT_INSTALL_TEST_HOOKS'
assert_contains "$receipt_dockerfile" 'evaluated-devel-source-artifact-transport-test'
exact_binding_target_body=$(make_target_body test-container-exact-installed-binding)
printf '%s\n' "$exact_binding_target_body" | grep -F -- '--network=none' >/dev/null ||
    fail 'exact installed binding lost its network-none boundary'
printf '%s\n' "$exact_binding_target_body" | grep -F -- \
    '--mount type=volume,destination=/var/lib/moguet-exact-installed-binding,volume-nocopy' >/dev/null ||
    fail 'exact installed binding lost its anonymous volume DB'
if printf '%s\n' "$exact_binding_target_body" | grep -E -- 'type=bind|/var/lib/pacman|--privileged' >/dev/null; then
    fail 'exact installed binding gained a host DB/privilege boundary'
fi
assert_contains "$test_targets_file" 'source/source_install.cpp'
assert_contains "$test_targets_file" \
    'MOGUET_ENABLE_REMOTE_AUR_CLEANUP_RUNNER_TEST_HOOKS'
assert_not_contains "$production_cmake_file" \
    'MOGUET_ENABLE_REMOTE_AUR_CLEANUP_RUNNER_TEST_HOOKS'
assert_contains "$production_source_runner" \
    'execute_prepared_remote_aur_cleanup_invocation('
assert_not_contains "$installed_source_fixture" \
    'execute_prepared_remote_aur_cleanup_invocation('
assert_contains "$installed_source_fixture" \
    'execute_source_build_package_base_with_cleanup_authority('

# The tracked fixture remains the Docker build input. Runtime cases consume its
# pre-build root-owned authority and keep generated metadata case-local.
tracked_srcinfo_raw=$tmp_dir/tracked-srcinfo.raw
if validation_capture_output "$tracked_srcinfo_raw" \
    find "$live_root/fixtures/local-package" \
    -mindepth 1 -name .SRCINFO -print; then
    :
else
    fixture_status=$?
    fail "tracked fixture path producer failed with status $fixture_status; raw=$tracked_srcinfo_raw"
fi
if [ -s "$tracked_srcinfo_raw" ]; then
    fail 'tracked live fixture must not contain .SRCINFO'
fi
assert_contains "$provider_runner" "assert_sentinel_absent \"\$current_case\""
assert_contains "$provider_runner" "assert_not_contains \"Running: 'git'\""
assert_contains "$provider_runner" "assert_not_contains \"Running: 'makepkg'\""
assert_contains "$provider_runner" \
    "assert_not_contains \"Running: 'sudo' 'pacman' '-U'\""
assert_contains "$provider_runner" "-name '.artifact-workspace~-*'"
assert_contains "$provider_runner" 'package_database_manifest'
assert_contains "$provider_runner" 'tracked_fixture_manifest'

assert_contains "$readme_file" 'real Arch repository metadata'
assert_contains "$readme_file" 'review-required failure'
assert_contains "$readme_file" '実package transactionは行わない'
assert_contains "$readme_file" 'Slice 3: real AUR'
assert_contains "$readme_file" 'real public AUR'
assert_contains "$readme_file" "AUR package **$AUR_CASE_PACKAGE_NAMEだけ**"
assert_contains "$readme_file" 'root-owned stagingへcopy'
assert_contains "$readme_file" 'reason **Explicit**'
assert_contains "$readme_file" 'version/reasonのbefore/afterが不変'
assert_contains "$readme_file" 'direct libarchive helper'
assert_contains "$readme_file" 'permission listingを'
assert_contains "$readme_file" 'ACL/xattr不在のauthorityには使わない'
assert_contains "$readme_file" 'exact multiset照合'
assert_contains "$readme_file" 'xattr、ACL、`pkgdesc` authority drift'
assert_contains "$readme_file" "!$AUR_CASE_README_PATH"
assert_contains "$readme_file" 'Slice 4: real local PKGBUILD install and release gate'
assert_contains "$readme_file" 'moguet --noedit build --local <directory>'
assert_contains "$readme_file" \
    "$PACKAGE_NAME $PACKAGE_VERSION\`は$ROOT_ARTIFACT_INSTALL_REASON"
assert_contains "$readme_file" 'root-owned read-only authorityへ固定'
assert_contains "$readme_file" 'make test-container-live`は単一のfail-fast recipe'
assert_contains "$readme_file" 'release-check`は`test-live-contract`'
assert_contains "$readme_file" 'image / layer cacheはhost localに残り得る'
assert_contains "$readme_file" 'host systemへpackage mutationは行わない'

printf '%s\n' 'live contract tests: all checks passed'
