#!/bin/sh
set -eu

module_test_binary=$1
repo_root=$(CDPATH='' cd "$(dirname "$0")/.." && pwd)
. "$repo_root/scripts/validation-status.sh"
tmp_dir=$(mktemp -d)

cleanup() {
    chmod 600 "$tmp_dir/unreadable.toml" 2>/dev/null || true
    rm -rf "$tmp_dir"
}
trap cleanup EXIT INT TERM

fail() {
    echo "$*" >&2
    exit 1
}

assert_line() {
    expected=$1
    file=$2
    if ! grep -Fx -- "$expected" "$file" >/dev/null; then
        echo "missing expected line: $expected" >&2
        sed -n '1,240p' "$file" >&2
        exit 1
    fi
}

assert_contains() {
    expected=$1
    file=$2
    if ! grep -F -- "$expected" "$file" >/dev/null; then
        echo "missing expected content: $expected" >&2
        sed -n '1,240p' "$file" >&2
        exit 1
    fi
}

assert_config_field_count() {
    file=$1
    if [ "$(wc -l < "$file")" -ne 4 ]; then
        echo "UserConfig output did not contain exactly four fields" >&2
        cat "$file" >&2
        exit 1
    fi
}

assert_config() {
    file=$1
    pkgbuild=$2
    diff=$3
    mode=$4
    assert_line "SCHEMA_VERSION=1" "$file"
    assert_line "REVIEW_PKGBUILD=$pkgbuild" "$file"
    assert_line "REVIEW_DIFF=$diff" "$file"
    assert_line "BUILD_MODE=$mode" "$file"
    assert_config_field_count "$file"
}

run_ok() {
    output_file=$1
    shift
    if ! "$module_test_binary" "$@" > "$output_file" 2>&1; then
        echo "user_config module test command failed: $*" >&2
        cat "$output_file" >&2
        exit 1
    fi
}

run_fail() {
    output_file=$1
    shift
    if ! validation_expect_status user-config-business-failure 1 \
        "$output_file" "$output_file" "$module_test_binary" "$@"; then
        cat "$output_file" >&2
        exit 1
    fi
}

assert_failure() {
    output_file=$1
    config_file=$2
    shift 2
    run_fail "$output_file" load "$config_file"
    assert_contains "$config_file" "$output_file"
    for expected in "$@"; do
        assert_contains "$expected" "$output_file"
    done
}

# Built-in defaultとmissing-file resultは同じtyped valuesで、pathを作成しない。
output=$tmp_dir/output
run_ok "$output" defaults
assert_config "$output" prompt prompt normal

run_ok "$output" composition
if [ -s "$output" ]; then
    fail "pure config composition emitted unexpected output"
fi

canonical_sample=$repo_root/sample/config.toml
[ -f "$canonical_sample" ] && [ ! -L "$canonical_sample" ] ||
    fail "canonical config sample must be a regular non-symlink: $canonical_sample"

canonical_sample_expected=$tmp_dir/canonical-sample.expected
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' ''
    printf '%s\n' '[review]'
    printf '%s\n' 'pkgbuild = "prompt"'
    printf '%s\n' 'diff = "prompt"'
    printf '%s\n' ''
    printf '%s\n' '[build]'
    printf '%s\n' 'mode = "normal"'
} > "$canonical_sample_expected"

canonical_sample_matches_expected() {
    candidate=$1
    cmp -s "$canonical_sample_expected" "$candidate"
}

assert_canonical_file_matches() {
    candidate=$1
    label=$2
    if canonical_sample_matches_expected "$candidate"; then
        return
    else
        comparison_status=$?
    fi

    case $comparison_status in
        1)
            diff -u "$canonical_sample_expected" "$candidate" >&2 || :
            fail "$label differs from expected canonical config"
            ;;
        *)
            fail "$label comparison failed with status $comparison_status"
            ;;
    esac
}

extract_configuration_toml_example() {
    readme=$1
    destination=$2
    if awk '
        /^<!-- parity:configuration -->[[:space:]]*$/ {
            in_configuration = 1
            next
        }
        in_configuration && /^<!-- parity:/ {
            in_configuration = 0
        }
        !in_configuration { next }
        /^```toml[[:space:]]*$/ {
            if (toml_blocks != 0 || in_toml) exit 2
            toml_blocks++
            in_toml = 1
            next
        }
        in_toml && /^```[[:space:]]*$/ {
            in_toml = 0
            closed_toml = 1
            next
        }
        in_toml {
            sub(/[[:space:]]+$/, "")
            if ($0 == "") {
                pending_blank_lines++
                next
            }
            while (pending_blank_lines > 0) {
                print ""
                pending_blank_lines--
            }
            print
        }
        END {
            if (toml_blocks != 1 || in_toml || !closed_toml) exit 2
        }
    ' "$readme" > "$destination"; then
        return
    else
        extraction_status=$?
    fi

    fail "unable to extract one configuration TOML example from $readme: status $extraction_status"
}

assert_canonical_file_matches "$canonical_sample" \
    "canonical config sample structure or defaults"

# Optional keys resolve to built-in defaults, so prove the raw completeness
# oracle rejects their omission independently of the production loader result.
missing_canonical_key_sample=$tmp_dir/canonical-sample-missing-pkgbuild.toml
sed '/^pkgbuild = "prompt"$/d' "$canonical_sample" > \
    "$missing_canonical_key_sample"
if grep -Fqx -- 'pkgbuild = "prompt"' "$missing_canonical_key_sample"; then
    fail "canonical sample negative fixture retained review.pkgbuild"
else
    missing_key_grep_status=$?
    case $missing_key_grep_status in
        1) ;;
        *)
            fail "canonical sample negative fixture inspection failed with status $missing_key_grep_status"
            ;;
    esac
fi
if canonical_sample_matches_expected "$missing_canonical_key_sample"; then
    fail "canonical sample completeness accepted missing review.pkgbuild"
else
    missing_key_comparison_status=$?
    case $missing_key_comparison_status in
        1) ;;
        *)
            fail "canonical sample negative comparison failed with status $missing_key_comparison_status"
            ;;
    esac
fi
printf '%s\n' \
    '  ok: rejected canonical sample missing optional review.pkgbuild'

for readme in "$repo_root/README.md" "$repo_root/README.ja.md"
do
    readme_example=$tmp_dir/$(basename "$readme").config.toml
    extract_configuration_toml_example "$readme" "$readme_example"
    assert_canonical_file_matches "$readme_example" \
        "$readme configuration TOML example"
done

cp "$canonical_sample" "$tmp_dir/canonical-sample.before"
run_ok "$output" load "$canonical_sample"
assert_config "$output" prompt prompt normal
cmp -s "$tmp_dir/canonical-sample.before" "$canonical_sample" ||
    fail "canonical config sample was modified by the production loader"

missing_parent=$tmp_dir/missing-parent
missing_config=$missing_parent/config.toml
run_ok "$output" load "$missing_config"
assert_config "$output" prompt prompt normal
if [ -e "$missing_parent" ] || [ -L "$missing_parent" ]; then
    fail "missing config parent was created: $missing_parent"
fi

existing_parent=$tmp_dir/existing-parent
mkdir "$existing_parent"
missing_leaf=$existing_parent/config.toml
run_ok "$output" load "$missing_leaf"
assert_config "$output" prompt prompt normal
if [ -e "$missing_leaf" ] || [ -L "$missing_leaf" ]; then
    fail "missing config file was created: $missing_leaf"
fi

minimal_config=$tmp_dir/minimal.toml
printf '%s\n' 'schema_version = 1' > "$minimal_config"
run_ok "$output" load "$minimal_config"
assert_config "$output" prompt prompt normal

full_config=$tmp_dir/full.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' '[review]'
    printf '%s\n' 'pkgbuild = "skip"'
    printf '%s\n' 'diff = "skip"'
    printf '%s\n' '[build]'
    printf '%s\n' 'mode = "clean"'
} > "$full_config"
cp "$full_config" "$tmp_dir/full.before"
run_ok "$output" load "$full_config"
assert_config "$output" skip skip clean
cmp -s "$tmp_dir/full.before" "$full_config" ||
    fail "valid config file was modified"

partial_config=$tmp_dir/partial.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' '[review]'
    printf '%s\n' 'diff = "skip"'
    printf '%s\n' '[build]'
} > "$partial_config"
run_ok "$output" load "$partial_config"
assert_config "$output" prompt skip normal

# Quoted keys, hexadecimal integer, dotted keys, literal/multiline stringsも
# TOMLとして同じtyped schemaを表す限り受理する。
toml_expressions_config=$tmp_dir/toml-expressions.toml
{
    printf '%s\n' '# comments are valid TOML'
    printf '%s\n' '"schema_version" = 0x1 # integer value 1'
    printf '%s\n' "review.pkgbuild = 'skip'"
    printf '%s\n' 'build.mode = """rebuild"""'
} > "$toml_expressions_config"
run_ok "$output" load "$toml_expressions_config"
assert_config "$output" skip prompt rebuild

missing_schema_config=$tmp_dir/missing-schema.toml
: > "$missing_schema_config"
assert_failure "$output" "$missing_schema_config" \
    "key 'schema_version'" "missing required key" "expected integer 1"

wrong_schema_type_config=$tmp_dir/wrong-schema-type.toml
printf '%s\n' 'schema_version = "1"' > "$wrong_schema_type_config"
assert_failure "$output" "$wrong_schema_type_config" \
    "key 'schema_version'" "line 1, column" "expected integer 1" "got string"

unsupported_schema_config=$tmp_dir/unsupported-schema.toml
printf '%s\n' 'schema_version = 2' > "$unsupported_schema_config"
assert_failure "$output" "$unsupported_schema_config" \
    "key 'schema_version'" "line 1, column" \
    "unsupported schema version 2" "expected integer 1"

syntax_error_config=$tmp_dir/syntax-error.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' '[review'
} > "$syntax_error_config"
assert_failure "$output" "$syntax_error_config" \
    "TOML parse error" "line 2, column"

duplicate_key_config=$tmp_dir/duplicate-key.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' 'schema_version = 1'
} > "$duplicate_key_config"
assert_failure "$output" "$duplicate_key_config" \
    "TOML parse error" "line 2, column"

duplicate_table_config=$tmp_dir/duplicate-table.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' '[review]'
    printf '%s\n' '[review]'
} > "$duplicate_table_config"
assert_failure "$output" "$duplicate_table_config" \
    "TOML parse error" "line 3, column"

scalar_table_conflict_config=$tmp_dir/scalar-table-conflict.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' 'review = "skip"'
    printf '%s\n' '[review]'
} > "$scalar_table_conflict_config"
assert_failure "$output" "$scalar_table_conflict_config" \
    "TOML parse error" "line 3, column"

unknown_top_key_config=$tmp_dir/unknown-top-key.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' 'unexpected = true'
} > "$unknown_top_key_config"
assert_failure "$output" "$unknown_top_key_config" \
    "top-level key 'unexpected'" "line 2, column 1" \
    "unknown top-level key" "schema_version, review, build"

unknown_top_section_config=$tmp_dir/unknown-top-section.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' '[unexpected]'
    printf '%s\n' 'value = true'
} > "$unknown_top_section_config"
assert_failure "$output" "$unknown_top_section_config" \
    "top-level section 'unexpected'" "line 2, column" \
    "unknown top-level section" "schema_version, review, build"

unknown_review_key_config=$tmp_dir/unknown-review-key.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' '[review]'
    printf '%s\n' 'unexpected = "skip"'
} > "$unknown_review_key_config"
assert_failure "$output" "$unknown_review_key_config" \
    "section 'review', key 'unexpected'" "line 3, column 1" \
    "unknown key" "pkgbuild, diff"

unknown_build_key_config=$tmp_dir/unknown-build-key.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' '[build]'
    printf '%s\n' 'unexpected = "normal"'
} > "$unknown_build_key_config"
assert_failure "$output" "$unknown_build_key_config" \
    "section 'build', key 'unexpected'" "line 3, column 1" \
    "unknown key" "mode"

review_type_config=$tmp_dir/review-type.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' 'review = "prompt"'
} > "$review_type_config"
assert_failure "$output" "$review_type_config" \
    "section 'review'" "line 2, column" "expected table" "got string"

build_type_config=$tmp_dir/build-type.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' 'build = ["normal"]'
} > "$build_type_config"
assert_failure "$output" "$build_type_config" \
    "section 'build'" "line 2, column" "expected table" "got array"

pkgbuild_type_config=$tmp_dir/pkgbuild-type.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' '[review]'
    printf '%s\n' 'pkgbuild = 1'
} > "$pkgbuild_type_config"
assert_failure "$output" "$pkgbuild_type_config" \
    "key 'review.pkgbuild'" "line 3, column" "expected string" \
    "accepted values: prompt, skip" "got integer"

diff_type_config=$tmp_dir/diff-type.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' '[review]'
    printf '%s\n' 'diff = false'
} > "$diff_type_config"
assert_failure "$output" "$diff_type_config" \
    "key 'review.diff'" "line 3, column" "expected string" \
    "accepted values: prompt, skip" "got boolean"

mode_type_config=$tmp_dir/mode-type.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' '[build]'
    printf '%s\n' 'mode = ["normal"]'
} > "$mode_type_config"
assert_failure "$output" "$mode_type_config" \
    "key 'build.mode'" "line 3, column" "expected string" \
    "accepted values: normal, rebuild, clean" "got array"

pkgbuild_enum_config=$tmp_dir/pkgbuild-enum.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' '[review]'
    printf '%s\n' 'pkgbuild = "always"'
} > "$pkgbuild_enum_config"
assert_failure "$output" "$pkgbuild_enum_config" \
    "key 'review.pkgbuild'" "line 3, column" "unsupported value" \
    "accepted values: prompt, skip"

diff_enum_config=$tmp_dir/diff-enum.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' '[review]'
    printf '%s\n' 'diff = "never"'
} > "$diff_enum_config"
assert_failure "$output" "$diff_enum_config" \
    "key 'review.diff'" "line 3, column" "unsupported value" \
    "accepted values: prompt, skip"

mode_enum_config=$tmp_dir/mode-enum.toml
{
    printf '%s\n' 'schema_version = 1'
    printf '%s\n' '[build]'
    printf '%s\n' 'mode = "cleanbuild"'
} > "$mode_enum_config"
cp "$mode_enum_config" "$tmp_dir/mode-enum.before"
assert_failure "$output" "$mode_enum_config" \
    "key 'build.mode'" "line 3, column" "unsupported value" \
    "accepted values: normal, rebuild, clean"
cmp -s "$tmp_dir/mode-enum.before" "$mode_enum_config" ||
    fail "invalid config file was modified"

unreadable_config=$tmp_dir/unreadable.toml
printf '%s\n' 'schema_version = 1' > "$unreadable_config"
chmod 000 "$unreadable_config"
if [ -r "$unreadable_config" ]; then
    echo "user_config tests: unreadable-file case skipped for privileged runner" >&2
else
    assert_failure "$output" "$unreadable_config" \
        "unable to open config file for reading"
fi

directory_config=$tmp_dir/config-directory
mkdir "$directory_config"
assert_failure "$output" "$directory_config" \
    "readable regular file" "got directory"

# Character deviceをempty config扱いしないことをMoguet-owned preflightで固定する。
if [ -e /dev/null ]; then
    assert_failure "$output" /dev/null \
        "readable regular file" "got character device"
fi

echo "user_config tests passed"
