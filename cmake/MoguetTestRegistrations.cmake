# CTest registration authority for the C++ test executables.  Build/link
# composition remains in MoguetTests.cmake; existing shell drivers remain the
# behavioral authority for CLI, fixture, and PTY scenarios.

macro(_moguet_add_direct_ctest test_name target_name)
    moguet_add_ctest(
        NAME "${test_name}"
        TARGETS "${target_name}"
        COMMAND "$<TARGET_FILE:${target_name}>"
    )
endmacro()

_moguet_add_direct_ctest(cpp.exact_artifact_transaction_protocol exact-artifact-transaction-protocol-test)
_moguet_add_direct_ctest(cpp.installed_package_record_observation installed-package-record-observation-test)
moguet_add_ctest(
    NAME cpp.exact_artifact_transaction_receipt
    TARGETS source-artifact-install-trusted-transport-test
    COMMAND "$<TARGET_FILE:source-artifact-install-trusted-transport-test>" --exact-receipt
)

# Focused executables with no runtime arguments or environment overrides.
_moguet_add_direct_ctest(cpp.interactive_confirmation interactive-confirmation-test)
_moguet_add_direct_ctest(cpp.xdg_paths xdg-paths-test)
_moguet_add_direct_ctest(cpp.xdg_directory_safety xdg-directory-safety-test)
_moguet_add_direct_ctest(
    cpp.xdg_generation_store
    xdg-generation-store-test
)
_moguet_add_direct_ctest(cpp.xdg_state_log xdg-state-log-test)
_moguet_add_direct_ctest(cpp.trusted_cache trusted-cache-test)
_moguet_add_direct_ctest(cpp.provider_selection provider-selection-test)
_moguet_add_direct_ctest(cpp.root_package_candidate root-package-candidate-test)
_moguet_add_direct_ctest(cpp.root_package_search root-package-search-test)
_moguet_add_direct_ctest(cpp.root_package_selection root-package-selection-test)
_moguet_add_direct_ctest(
    cpp.root_package_route_projection
    root-package-route-projection-test
)
_moguet_add_direct_ctest(cpp.local_source_root local-source-root-test)
_moguet_add_direct_ctest(
    cpp.local_dependency_plan_projection
    local-dependency-plan-projection-test
)
_moguet_add_direct_ctest(cpp.local_source_workspace local-source-workspace-test)
_moguet_add_direct_ctest(cpp.local_source_build local-source-build-test)
_moguet_add_direct_ctest(cpp.package_identifier package-identifier-test)
_moguet_add_direct_ctest(
    cpp.source_package_identity
    source-package-identity-test
)
_moguet_add_direct_ctest(
    cpp.installed_artifact_binding
    installed-artifact-binding-test
)
_moguet_add_direct_ctest(
    cpp.devel_build_provenance
    devel-build-provenance-test
)
_moguet_add_direct_ctest(
    cpp.devel_build_provenance_store
    devel-build-provenance-store-test
)
_moguet_add_direct_ctest(
    cpp.git_remote_revision_observer
    git-remote-revision-observer-test
)
moguet_add_ctest(
    NAME cpp.git_remote_revision_observer_integration
    TARGETS git-remote-revision-observer-test
    COMMAND
        sh
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-git-remote-revision-observer-integration.sh"
        "$<TARGET_FILE:git-remote-revision-observer-test>"
)
set_tests_properties(
    cpp.git_remote_revision_observer_integration
    PROPERTIES TIMEOUT 180
)
_moguet_add_direct_ctest(
    cpp.source_package_identity_projection
    source-package-identity-projection-test
)
_moguet_add_direct_ctest(
    cpp.source_package_compatibility
    source-package-compatibility-test
)
_moguet_add_direct_ctest(
    cpp.makepkg_devel_phase_characterization
    makepkg-devel-phase-characterization-test
)
_moguet_add_direct_ctest(
    cpp.invocation_owned_cleanup_model
    invocation-owned-cleanup-model-test
)
moguet_add_ctest(
    NAME cpp.remote_aur_cleanup_collector
    TARGETS invocation-owned-cleanup-model-test
    COMMAND
        "$<TARGET_FILE:invocation-owned-cleanup-model-test>"
        --collector-only
)
_moguet_add_direct_ctest(cpp.reviewed_source_state reviewed-source-state-test)
_moguet_add_direct_ctest(
    cpp.reviewed_source_state_store
    reviewed-source-state-store-test
)
_moguet_add_direct_ctest(
    cpp.reviewed_source_lifecycle
    reviewed-source-lifecycle-test
)
_moguet_add_direct_ctest(
    cpp.reviewed_source_acceptance
    reviewed-source-acceptance-test
)
_moguet_add_direct_ctest(
    cpp.reviewed_source_pinned_build
    reviewed-source-pinned-build-test
)
_moguet_add_direct_ctest(
    cpp.invocation_owned_source_build_context
    invocation-owned-source-build-context-test
)
_moguet_add_direct_ctest(
    cpp.evaluated_devel_source_artifact_transport
    evaluated-devel-source-artifact-transport-test
)
set_tests_properties(cpp.evaluated_devel_source_artifact_transport PROPERTIES TIMEOUT 240)
moguet_add_ctest(
    NAME cpp.exact_installed_binding
    TARGETS evaluated-devel-source-artifact-transport-test
    COMMAND "$<TARGET_FILE:evaluated-devel-source-artifact-transport-test>" --exact-installed-binding
)
set_tests_properties(cpp.exact_installed_binding PROPERTIES TIMEOUT 480)
_moguet_add_direct_ctest(
    cpp.evaluated_devel_source_build
    evaluated-devel-source-build-test
)
set_tests_properties(
    cpp.evaluated_devel_source_build
    PROPERTIES TIMEOUT 180
)
_moguet_add_direct_ctest(
    cpp.reviewed_source_projection
    reviewed-source-projection-test
)
_moguet_add_direct_ctest(cpp.reviewed_source_review reviewed-source-review-test)
_moguet_add_direct_ctest(cpp.reviewed_source_patch reviewed-source-patch-test)
_moguet_add_direct_ctest(
    cpp.reviewed_source_presentation
    reviewed-source-presentation-test
)
_moguet_add_direct_ctest(cpp.reviewed_source_git reviewed-source-git-test)
_moguet_add_direct_ctest(cpp.shell_words shell-words-test)
_moguet_add_direct_ctest(cpp.artifact_identity artifact-identity-test)
_moguet_add_direct_ctest(
    cpp.multiple_artifact_identity
    multiple-artifact-identity-test
)
_moguet_add_direct_ctest(
    cpp.package_base_artifact_install_plan
    package-base-artifact-install-plan-test
)
_moguet_add_direct_ctest(
    cpp.artifact_install_executor
    artifact-install-executor-test
)
_moguet_add_direct_ctest(
    cpp.package_base_artifact_install_executor
    package-base-artifact-install-executor-test
)
_moguet_add_direct_ctest(
    cpp.source_artifact_install_trusted_transport
    source-artifact-install-trusted-transport-test
)
_moguet_add_direct_ctest(cpp.separated_source_build separated-source-build-test)
_moguet_add_direct_ctest(
    cpp.separated_package_base_source_build
    separated-package-base-source-build-test
)
_moguet_add_direct_ctest(cpp.process_capture process-capture-test)
_moguet_add_direct_ctest(cpp.bounded_process bounded-process-test)
_moguet_add_direct_ctest(cpp.process_stdin_fd process-stdin-fd-test)
_moguet_add_direct_ctest(cpp.aur_update_plan aur-update-plan-test)
_moguet_add_direct_ctest(cpp.upgrade_all_plan upgrade-all-plan-test)
_moguet_add_direct_ctest(cpp.system_source_upgrade system-source-upgrade-test)
_moguet_add_direct_ctest(cpp.aur_update_query aur-update-query-test)
_moguet_add_direct_ctest(
    cpp.aur_update_execution_preflight
    aur-update-execution-preflight-test
)
_moguet_add_direct_ctest(
    cpp.aur_update_execution_preparation
    aur-update-execution-preparation-test
)
_moguet_add_direct_ctest(
    cpp.aur_update_execution_runner
    aur-update-execution-runner-test
)
_moguet_add_direct_ctest(
    cpp.aur_update_operation_result
    aur-update-operation-result-test
)
_moguet_add_direct_ctest(
    cpp.filtered_aur_update_operation
    filtered-aur-update-operation-test
)
_moguet_add_direct_ctest(cpp.upgrade_all_operation upgrade-all-operation-test)
_moguet_add_direct_ctest(cpp.cli_diagnostic_model cli-diagnostic-model-test)
_moguet_add_direct_ctest(cpp.runtime_cli_connection runtime-cli-connection-test)
_moguet_add_direct_ctest(cpp.dependency_plan_model dependency-plan-model-test)
_moguet_add_direct_ctest(
    cpp.build_plan_artifact_target_projection
    build-plan-artifact-target-projection-test
)
_moguet_add_direct_ctest(
    cpp.unified_plan_observation
    unified-plan-observation-test
)
_moguet_add_direct_ctest(
    cpp.unified_plan_projection
    unified-plan-projection-test
)
_moguet_add_direct_ctest(cpp.artifact_install_plan artifact-install-plan-test)
_moguet_add_direct_ctest(
    cpp.artifact_selection_model
    artifact-selection-model-test
)
_moguet_add_direct_ctest(
    cpp.artifact_identity_selection
    artifact-identity-selection-test
)
_moguet_add_direct_ctest(cpp.package_metadata package-metadata-test)
_moguet_add_direct_ctest(
    cpp.provider_installed_state
    provider-installed-state-test
)
_moguet_add_direct_ctest(cpp.dependency_constraint dependency-constraint-test)
_moguet_add_direct_ctest(
    cpp.cross_source_version_lock
    cross-source-version-lock-test
)
_moguet_add_direct_ctest(cpp.package_relation package-relation-test)
_moguet_add_direct_ctest(
    cpp.package_relation_observation
    package-relation-observation-test
)
_moguet_add_direct_ctest(
    cpp.package_relation_assessment
    package-relation-assessment-test
)
_moguet_add_direct_ctest(
    cpp.package_constraint_metadata
    package-constraint-metadata-test
)
_moguet_add_direct_ctest(
    cpp.aur_constraint_metadata
    aur-constraint-metadata-test
)
_moguet_add_direct_ctest(
    cpp.package_metadata_integration
    package-metadata-integration-test
)

# Direct executables with target-specific arguments or environment.
moguet_add_ctest(
    NAME cpp.application_identity
    TARGETS application-identity-test
    COMMAND "$<TARGET_FILE:application-identity-test>" "${MOGUET_VERSION}"
)

moguet_add_ctest(
    NAME cpp.local_package_metadata
    TARGETS local-package-metadata-test
    COMMAND
        "$<TARGET_FILE:local-package-metadata-test>"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/local-package-metadata"
)

moguet_add_ctest(
    NAME cpp.source_environment
    TARGETS source-environment-test
    COMMAND
        "$<TARGET_FILE:source-environment-test>"
        "${CMAKE_CURRENT_BINARY_DIR}/tests/source-environment-fixture/config/moguet/source-build.d"
    ENVIRONMENT
        "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/tests/source-environment-fixture/config"
        "HOME=${CMAKE_CURRENT_BINARY_DIR}/tests/source-environment-fixture/home"
)

moguet_add_ctest(
    NAME cpp.artifact_workspace
    TARGETS artifact-workspace-test
    COMMAND "$<TARGET_FILE:artifact-workspace-test>"
    ENVIRONMENT
        "MOGUET_TEST_MAKEPKG_STUB=${CMAKE_CURRENT_SOURCE_DIR}/tests/stubs/makepkg"
)

moguet_add_ctest(
    NAME cpp.multiple_artifact_workspace
    TARGETS multiple-artifact-workspace-test
    COMMAND "$<TARGET_FILE:multiple-artifact-workspace-test>"
    ENVIRONMENT
        "MOGUET_TEST_MAKEPKG_STUB=${CMAKE_CURRENT_SOURCE_DIR}/tests/stubs/makepkg"
)

moguet_add_ctest(
    NAME cpp.makepkg_assignment_precedence
    TARGETS makepkg-assignment-precedence-test
    COMMAND
        sh -c
        [=[
test -x /usr/bin/makepkg || {
    echo 'error: /usr/bin/makepkg is required for assignment precedence validation' >&2
    exit 1
}
test -x /usr/bin/bsdtar || {
    echo 'error: /usr/bin/bsdtar is required for assignment precedence validation' >&2
    exit 1
}
exec "$1"
]=]
        sh "$<TARGET_FILE:makepkg-assignment-precedence-test>"
)

set(
    MOGUET_PRODUCTION_SOURCE_BUILD_TEST_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/tests/production-source-build-preferences"
)
file(MAKE_DIRECTORY "${MOGUET_PRODUCTION_SOURCE_BUILD_TEST_ROOT}/home")
moguet_add_ctest(
    NAME cpp.production_source_build
    TARGETS production-source-build-test
    COMMAND "$<TARGET_FILE:production-source-build-test>"
    ENVIRONMENT
        "XDG_CONFIG_HOME=${MOGUET_PRODUCTION_SOURCE_BUILD_TEST_ROOT}/config"
        "HOME=${MOGUET_PRODUCTION_SOURCE_BUILD_TEST_ROOT}/home"
)

moguet_add_ctest(
    NAME cpp.aur_update_execution_preparation.integration
    TARGETS aur-update-execution-preparation-integration-test
    COMMAND
        "$<TARGET_FILE:aur-update-execution-preparation-integration-test>"
        "${CMAKE_CURRENT_BINARY_DIR}/tests/aur-update-execution-preparation-fixture/config/moguet/source-build.d"
    ENVIRONMENT
        "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/tests/aur-update-execution-preparation-fixture/config"
        "HOME=${CMAKE_CURRENT_BINARY_DIR}/tests/aur-update-execution-preparation-fixture/home"
)

moguet_add_ctest(
    NAME cpp.unified_plan_renderer
    TARGETS unified-plan-renderer-test
    COMMAND "$<TARGET_FILE:unified-plan-renderer-test>"
    ENVIRONMENT "LC_ALL=C" "LANGUAGE="
)

# The legacy preflight integration recipe invokes the same executable with five
# independently identifiable scenarios.
foreach(_moguet_preflight_case IN ITEMS
    simple
    repository-failure
    aur-failure
    relation-assessment
    relation-query-failure
)
    moguet_add_ctest(
        NAME "cpp.aur_update_execution_preflight_integration.${_moguet_preflight_case}"
        TARGETS aur-update-execution-preflight-integration-test
        COMMAND
            "$<TARGET_FILE:aur-update-execution-preflight-integration-test>"
            "${_moguet_preflight_case}"
    )
endforeach()
unset(_moguet_preflight_case)

# Repository-query cases remain separate CTest entries so a failing contract is
# visible without inspecting a loop's combined output.
foreach(_moguet_repository_query_case IN ITEMS
    candidate-value-contract
    configured-order
    split-package-base
    confirmed-not-found
    malformed-package-base
    returned-child-mismatch
    present-later-failure
    absent-later-failure
    unrelated-malformed-exact
    provider-capabilities
    provider-partial-failure
    repository-named-aur
    configuration-failure
    installed-exact-states
)
    moguet_add_ctest(
        NAME "cpp.repository_query.${_moguet_repository_query_case}"
        TARGETS repository-query-test
        COMMAND
            "$<TARGET_FILE:repository-query-test>"
            "${_moguet_repository_query_case}"
    )
endforeach()
unset(_moguet_repository_query_case)

# Existing shell and PTY drivers remain the behavioral authority.  TARGETS
# lists only C++ test executables; the production `moguet` target used by the
# runtime-identity driver is intentionally outside the 98-target test ledger.
moguet_add_ctest(
    NAME localization.contract
    TARGETS
        localization-test
        localization-missing-catalog-test
        moguet-cli-localization-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-localization.sh"
        "$<TARGET_FILE:localization-test>"
        "$<TARGET_FILE:localization-missing-catalog-test>"
        "${CMAKE_CURRENT_BINARY_DIR}/locale"
        "${CMAKE_CURRENT_BINARY_DIR}/tests/missing-locale"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/localization/invalid-format.po"
        "${MOGUET_MSGFMT_EXECUTABLE}"
        "$<TARGET_FILE:moguet-cli-localization-test>"
)

moguet_add_ctest(
    NAME cli.runtime_identity
    TARGETS
        moguet-root-execution-identity-test
        moguet-app-config-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-runtime-identity.sh"
        "$<TARGET_FILE:moguet>"
        "$<TARGET_FILE:moguet-root-execution-identity-test>"
        "$<TARGET_FILE:moguet-app-config-test>"
)

moguet_add_ctest(
    NAME config.app
    TARGETS app-config-test moguet-app-config-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-app-config.sh"
        "$<TARGET_FILE:app-config-test>"
        "$<TARGET_FILE:moguet-app-config-test>"
)

moguet_add_ctest(
    NAME config.user
    TARGETS user-config-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-user-config.sh"
        "$<TARGET_FILE:user-config-test>"
)

moguet_add_ctest(
    NAME cpp.reviewed_source_production_connection
    TARGETS reviewed-source-production-connection-test
    COMMAND
        sh -c
        [=[
printf 'y\ny\ny\nn\ny\ny\ny\ny\ny\ny\ny\n' |
    script -qec "$1" /dev/null
]=]
        sh "$<TARGET_FILE:reviewed-source-production-connection-test>"
)

moguet_add_ctest(
    NAME cli.aur_update_command
    TARGETS moguet-aur-update-command-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-aur-update-command.sh"
        "$<TARGET_FILE:moguet-aur-update-command-test>"
)

moguet_add_ctest(
    NAME cli.upgrade_all_command
    TARGETS moguet-upgrade-all-command-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-upgrade-all-command.sh"
        "$<TARGET_FILE:moguet-upgrade-all-command-test>"
)

moguet_add_ctest(
    NAME cli.conflicts_replaces
    TARGETS moguet-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-conflicts-replaces.sh"
        "$<TARGET_FILE:moguet-test>"
)

moguet_add_ctest(
    NAME cli.aur_rpc_validation
    TARGETS
        moguet-aur-rpc-validation-test
        aur-rpc-envelope-validation-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-aur-rpc-validation.sh"
        "$<TARGET_FILE:moguet-aur-rpc-validation-test>"
        "$<TARGET_FILE:aur-rpc-envelope-validation-test>"
)

moguet_add_ctest(
    NAME cli.parser
    TARGETS moguet-aur-rpc-validation-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-cli-parser.sh"
        "$<TARGET_FILE:moguet-aur-rpc-validation-test>"
)

moguet_add_ctest(
    NAME cli.dry_run
    TARGETS moguet-test moguet-aur-rpc-validation-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-dry-run-command.sh"
        "$<TARGET_FILE:moguet-test>"
        "$<TARGET_FILE:moguet-aur-rpc-validation-test>"
)

moguet_add_ctest(
    NAME cli.commands_inspect
    TARGETS moguet-commands-inspect-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-commands-inspect.sh"
        "$<TARGET_FILE:moguet-commands-inspect-test>"
)

moguet_add_ctest(
    NAME cli.commands_source_maintenance
    TARGETS
        moguet-source-install-characterization-test
        moguet-upgrade-baseline-metadata-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-commands-source-maintenance.sh"
        "$<TARGET_FILE:moguet-upgrade-baseline-metadata-test>"
        "$<TARGET_FILE:moguet-source-install-characterization-test>"
        "$<TARGET_FILE:moguet-upgrade-baseline-metadata-test>"
)

moguet_add_ctest(
    NAME cli.commands_sync
    TARGETS moguet-commands-sync-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-commands-sync.sh"
        "$<TARGET_FILE:moguet-commands-sync-test>"
)

moguet_add_ctest(
    NAME cli.pacman_routing
    TARGETS moguet-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-pacman-routing.sh"
        "$<TARGET_FILE:moguet-test>"
)

moguet_add_ctest(
    NAME cli.build_cache_symlink
    TARGETS moguet-aur-rpc-validation-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-build-cache-symlink.sh"
        "$<TARGET_FILE:moguet-aur-rpc-validation-test>"
)

moguet_add_ctest(
    NAME cli.source_build
    TARGETS
        moguet-aur-rpc-validation-test
        moguet-upgrade-baseline-metadata-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-source-build.sh"
        "$<TARGET_FILE:moguet-aur-rpc-validation-test>"
        "$<TARGET_FILE:moguet-upgrade-baseline-metadata-test>"
        "$<TARGET_FILE:moguet-upgrade-baseline-metadata-test>"
)

moguet_add_ctest(
    NAME cli.source_selection
    TARGETS moguet-aur-rpc-validation-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-source-selection.sh"
        "$<TARGET_FILE:moguet-aur-rpc-validation-test>"
)

moguet_add_ctest(
    NAME cli.needed_contract
    TARGETS moguet-aur-rpc-validation-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-needed-contract.sh"
        "$<TARGET_FILE:moguet-aur-rpc-validation-test>"
)

moguet_add_ctest(
    NAME cli.pkgbuild_export
    TARGETS moguet-test
    COMMAND
        sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-pkgbuild-export.sh"
        "$<TARGET_FILE:moguet-test>"
)

# Link-symbol firewalls inspect the exact single-source OBJECT targets consumed
# by the production executable, then self-check the ERE against representative
# forbidden symbols.  A producer failure is still handled by the existing
# fail-closed script.
if(DEFINED ENV{NM} AND NOT "$ENV{NM}" STREQUAL "")
    set(_moguet_nm_command "$ENV{NM}")
elseif(CMAKE_NM)
    set(_moguet_nm_command "${CMAKE_NM}")
else()
    set(_moguet_nm_command nm)
endif()

set(
    _moguet_projection_forbidden_symbol_pattern
    [=[(^|[^[:alnum:]_])(resolve_|execute_|run_command|capture_command|cmd_|shell_words::|Process::|prepare_production_source_build_invocation|prepare_aur_source_build_work_items|run_explicit_process|capture_explicit_process_output_raw|exec_command|command_status|prepare_artifact_install|prepare_package_base_artifact_install|prepare_smart_source_build_work_item|prepare_resolved_source_build_work_item|argv)]=]
)
set(_moguet_projection_probe_symbols
    resolve_build_plan
    execute_source_build_typed
    shell_words::quote
    argv
    run_explicit_process
    capture_explicit_process_output_raw
    exec_command
    command_status
    prepare_artifact_install
    prepare_package_base_artifact_install
    prepare_smart_source_build_work_item
    prepare_resolved_source_build_work_item
)

set(
    _moguet_renderer_forbidden_symbol_pattern
    [=[(^|[^[:alnum:]_])(resolve_|evaluate_consumer_dependency_requirement|select_provider|make_provider_selection_session|provider_selection_callback|execute_|run_command|capture_command|cmd_|shell_words::|Process::|prepare_production_source_build_invocation|prepare_aur_source_build_work_items|run_explicit_process|capture_explicit_process_output_raw|exec_command|command_status|prepare_artifact_install|prepare_package_base_artifact_install|prepare_smart_source_build_work_item|prepare_resolved_source_build_work_item|argv)]=]
)
set(_moguet_renderer_probe_symbols
    resolve_build_plan
    evaluate_consumer_dependency_requirement
    select_provider
    make_provider_selection_session
    provider_selection_callback
    execute_source_build_typed
    shell_words::quote
    argv
    run_explicit_process
    capture_explicit_process_output_raw
    exec_command
    command_status
    prepare_artifact_install
    prepare_package_base_artifact_install
    prepare_smart_source_build_work_item
    prepare_resolved_source_build_work_item
)

set(_moguet_nm_firewall_driver [=[
set -eu
nm_command=$1
checker=$2
object_file=$3
pattern=$4
label=$5
shift 5
NM="$nm_command" sh "$checker" "$object_file" "$pattern" "$label"
for symbol in "$@"
do
    if ! printf '                 U %s\n' "$symbol" | grep -Eq "$pattern"
    then
        printf 'error: %s firewall misses representative symbol %s\n' \
            "$label" "$symbol" >&2
        exit 1
    fi
done
]=])

moguet_add_ctest(
    NAME build_contract.unified_plan_projection_link_firewall
    COMMAND
        sh -c "${_moguet_nm_firewall_driver}" sh
        "${_moguet_nm_command}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-nm-symbol-firewall.sh"
        "$<TARGET_OBJECTS:moguet_unified_plan_projection_object>"
        "${_moguet_projection_forbidden_symbol_pattern}"
        "unified plan projection object"
        ${_moguet_projection_probe_symbols}
)

moguet_add_ctest(
    NAME build_contract.unified_plan_renderer_link_firewall
    COMMAND
        sh -c "${_moguet_nm_firewall_driver}" sh
        "${_moguet_nm_command}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-nm-symbol-firewall.sh"
        "$<TARGET_OBJECTS:moguet_unified_plan_renderer_object>"
        "${_moguet_renderer_forbidden_symbol_pattern}"
        "unified plan renderer object"
        ${_moguet_renderer_probe_symbols}
)

# Keep the baseline-success + ten expected-private-diagnostic contract under
# the configured CMake compiler, launcher, and compile profile.  Files carry
# raw external values without re-encoding semicolons through `cmake -D`.
set(
    _moguet_negative_compile_state_dir
    "${CMAKE_CURRENT_BINARY_DIR}/moguet-negative-compile"
)
file(MAKE_DIRECTORY "${_moguet_negative_compile_state_dir}")
set(
    _moguet_negative_compile_cxx_file
    "${_moguet_negative_compile_state_dir}/cxx.txt"
)
set(
    _moguet_negative_compile_cxx_arg1_file
    "${_moguet_negative_compile_state_dir}/cxx-arg1.txt"
)
set(
    _moguet_negative_compile_launcher_file
    "${_moguet_negative_compile_state_dir}/launcher.txt"
)
set(
    _moguet_negative_compile_cppflags_file
    "${_moguet_negative_compile_state_dir}/cppflags.txt"
)
set(
    _moguet_negative_compile_cxxflags_file
    "${_moguet_negative_compile_state_dir}/cxxflags.txt"
)
set(
    _moguet_negative_compile_configuration_flags_file
    "${_moguet_negative_compile_state_dir}/configuration-flags.txt"
)
set(
    _moguet_negative_compile_project_options_file
    "${_moguet_negative_compile_state_dir}/project-options.txt"
)
file(WRITE "${_moguet_negative_compile_cxx_file}" "${CMAKE_CXX_COMPILER}")
file(
    WRITE
    "${_moguet_negative_compile_cxx_arg1_file}"
    "${CMAKE_CXX_COMPILER_ARG1}"
)
file(WRITE "${_moguet_negative_compile_launcher_file}" "")
foreach(_moguet_launcher_argument IN LISTS CMAKE_CXX_COMPILER_LAUNCHER)
    file(
        APPEND
        "${_moguet_negative_compile_launcher_file}"
        "${_moguet_launcher_argument}\n"
    )
endforeach()
file(
    WRITE
    "${_moguet_negative_compile_cppflags_file}"
    "${MOGUET_CPPFLAGS}"
)
file(
    WRITE
    "${_moguet_negative_compile_cxxflags_file}"
    "${CMAKE_CXX_FLAGS}"
)
set(_moguet_negative_compile_configuration_flags "")
if(NOT CMAKE_BUILD_TYPE STREQUAL "")
    string(
        TOUPPER
        "${CMAKE_BUILD_TYPE}"
        _moguet_negative_compile_configuration
    )
    set(
        _moguet_negative_compile_configuration_flags
        "${CMAKE_CXX_FLAGS_${_moguet_negative_compile_configuration}}"
    )
endif()
file(
    WRITE
    "${_moguet_negative_compile_configuration_flags_file}"
    "${_moguet_negative_compile_configuration_flags}"
)

if(NOT CMAKE_CXX20_STANDARD_COMPILE_OPTION)
    message(
        FATAL_ERROR
        "The configured compiler has no strict C++20 compile option"
    )
endif()
set(
    _moguet_negative_compile_project_options
    "${CMAKE_CXX20_STANDARD_COMPILE_OPTION}"
    -Wall
    -Wextra
)
if(
    MOGUET_ENABLE_DEFAULT_COMPILE_OPTIONS
    AND CMAKE_CXX_FLAGS STREQUAL ""
    AND CMAKE_BUILD_TYPE STREQUAL ""
    AND NOT CMAKE_CONFIGURATION_TYPES
)
    list(
        APPEND
        _moguet_negative_compile_project_options
        -O2
        -pipe
    )
endif()
list(
    APPEND
    _moguet_negative_compile_project_options
    "-DMOGUET_VERSION=\"${MOGUET_VERSION}\""
    "-I${MOGUET_GENERATED_INCLUDE_DIR}"
    "-I${CMAKE_CURRENT_SOURCE_DIR}/source"
)
file(WRITE "${_moguet_negative_compile_project_options_file}" "")
foreach(
    _moguet_negative_compile_project_option
    IN LISTS _moguet_negative_compile_project_options
)
    file(
        APPEND
        "${_moguet_negative_compile_project_options_file}"
        "${_moguet_negative_compile_project_option}\n"
    )
endforeach()

moguet_add_ctest(
    NAME build_contract.reviewed_source_authority_negative
    COMMAND
        "${CMAKE_COMMAND}"
        "-DMOGUET_NEGATIVE_COMPILE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/reviewed_source_authority_negative_test.cpp"
        "-DMOGUET_NEGATIVE_COMPILE_CXX_FILE=${_moguet_negative_compile_cxx_file}"
        "-DMOGUET_NEGATIVE_COMPILE_CXX_ARG1_FILE=${_moguet_negative_compile_cxx_arg1_file}"
        "-DMOGUET_NEGATIVE_COMPILE_LAUNCHER_FILE=${_moguet_negative_compile_launcher_file}"
        "-DMOGUET_NEGATIVE_COMPILE_CPPFLAGS_FILE=${_moguet_negative_compile_cppflags_file}"
        "-DMOGUET_NEGATIVE_COMPILE_CXXFLAGS_FILE=${_moguet_negative_compile_cxxflags_file}"
        "-DMOGUET_NEGATIVE_COMPILE_CONFIGURATION_FLAGS_FILE=${_moguet_negative_compile_configuration_flags_file}"
        "-DMOGUET_NEGATIVE_COMPILE_PROJECT_OPTIONS_FILE=${_moguet_negative_compile_project_options_file}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MoguetNegativeCompile.cmake"
)

unset(_moguet_negative_compile_state_dir)
unset(_moguet_negative_compile_cxx_file)
unset(_moguet_negative_compile_cxx_arg1_file)
unset(_moguet_negative_compile_launcher_file)
unset(_moguet_negative_compile_cppflags_file)
unset(_moguet_negative_compile_cxxflags_file)
unset(_moguet_negative_compile_configuration_flags)
unset(_moguet_negative_compile_configuration)
unset(_moguet_negative_compile_configuration_flags_file)
unset(_moguet_negative_compile_project_options)
unset(_moguet_negative_compile_project_option)
unset(_moguet_negative_compile_project_options_file)
unset(_moguet_launcher_argument)
unset(_moguet_nm_command)
unset(_moguet_nm_firewall_driver)
unset(_moguet_projection_forbidden_symbol_pattern)
unset(_moguet_projection_probe_symbols)
unset(_moguet_renderer_forbidden_symbol_pattern)
unset(_moguet_renderer_probe_symbols)
