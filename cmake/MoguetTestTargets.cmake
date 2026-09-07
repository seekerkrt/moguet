# C++ test executable graph migrated from the legacy Make authority. Keep
# source closures explicit: target-local stubs replace only the production
# owners excluded by the corresponding list firewall.

set(_moguet_test_source_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/source")
set(_moguet_test_support_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests")
set(
    _moguet_test_alpm_stub_include_dir
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/stubs/package-metadata"
)

function(_moguet_test_production_complement output_variable)
    set(_moguet_forbidden_sources ${MOGUET_PRODUCTION_SOURCES})
    list(REMOVE_ITEM _moguet_forbidden_sources ${ARGN})
    set(${output_variable} ${_moguet_forbidden_sources} PARENT_SCOPE)
endfunction()

set(
    _moguet_aur_update_command_test_sources
    ${MOGUET_PRODUCTION_SOURCES}
)
list(
    REMOVE_ITEM _moguet_aur_update_command_test_sources
    source/artifact_archive_metadata.cpp
    source/aur_update_query.cpp
    source/aur_update_execution_preflight.cpp
    source/aur_update_execution_preparation.cpp
    source/aur_update_execution_runner.cpp
    source/aur_update_operation_result.cpp
    source/filtered_aur_update_operation.cpp
    source/upgrade_all_operation.cpp
)
list(
    APPEND _moguet_aur_update_command_test_sources
    tests/stubs/artifact-identity/archive_metadata_stub.cpp
    tests/stubs/aur-update-command/operation_stub.cpp
    tests/stubs/upgrade-all-command/operation_stub.cpp
    tests/stubs/package-metadata/alpm_stub.cpp
)
_moguet_test_production_complement(
    _moguet_aur_update_command_forbidden_sources
    ${_moguet_aur_update_command_test_sources}
)
moguet_add_cpp_test(
    moguet-aur-update-command-test
    FIREWALL
    ALPM_COMPILE
    CURL
    SOURCES ${_moguet_aur_update_command_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_TEST_CONFIG_PATH
        MOGUET_ENABLE_SYSTEM_AUR_UPDATE_PRESENTATION_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_aur_update_command_forbidden_sources}
)

set(
    _moguet_upgrade_all_command_test_sources
    ${MOGUET_PRODUCTION_SOURCES}
)
list(
    REMOVE_ITEM _moguet_upgrade_all_command_test_sources
    source/artifact_archive_metadata.cpp
    source/upgrade_all_operation.cpp
)
list(
    APPEND _moguet_upgrade_all_command_test_sources
    tests/stubs/artifact-identity/archive_metadata_stub.cpp
    tests/stubs/upgrade-all-command/operation_stub.cpp
    tests/stubs/package-metadata/alpm_stub.cpp
)
_moguet_test_production_complement(
    _moguet_upgrade_all_command_forbidden_sources
    ${_moguet_upgrade_all_command_test_sources}
)
moguet_add_cpp_test(
    moguet-upgrade-all-command-test
    FIREWALL
    ALPM_COMPILE
    CURL
    SOURCES ${_moguet_upgrade_all_command_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_TEST_CONFIG_PATH
        "MOGUET_LOCALE_DIRECTORY=\"${CMAKE_CURRENT_BINARY_DIR}/locale\""
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_upgrade_all_command_forbidden_sources}
)

set(_moguet_commands_sync_test_sources ${MOGUET_PRODUCTION_SOURCES})
list(
    REMOVE_ITEM _moguet_commands_sync_test_sources
    source/artifact_archive_metadata.cpp
    source/aur_rpc.cpp
    source/root_package_search.cpp
)
list(
    APPEND _moguet_commands_sync_test_sources
    tests/stubs/artifact-identity/archive_metadata_stub.cpp
    tests/stubs/commands-sync/aur_rpc_stub.cpp
    tests/stubs/commands-sync/root_package_search_stub.cpp
    tests/stubs/package-metadata/alpm_stub.cpp
)
_moguet_test_production_complement(
    _moguet_commands_sync_forbidden_sources
    ${_moguet_commands_sync_test_sources}
)
moguet_add_cpp_test(
    moguet-commands-sync-test
    FIREWALL
    ALPM_COMPILE
    CURL
    SOURCES ${_moguet_commands_sync_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_TEST_CONFIG_PATH
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_commands_sync_forbidden_sources}
)

set(_moguet_commands_inspect_test_sources ${MOGUET_PRODUCTION_SOURCES})
list(
    REMOVE_ITEM _moguet_commands_inspect_test_sources
    source/artifact_archive_metadata.cpp
    source/aur_rpc.cpp
    source/repository_query.cpp
)
list(
    APPEND _moguet_commands_inspect_test_sources
    tests/commands_inspect_aur_stub.cpp
    tests/stubs/artifact-identity/archive_metadata_stub.cpp
    tests/stubs/commands-inspect/repository_query_stub.cpp
    tests/stubs/package-metadata/alpm_stub.cpp
)
_moguet_test_production_complement(
    _moguet_commands_inspect_forbidden_sources
    ${_moguet_commands_inspect_test_sources}
)
moguet_add_cpp_test(
    moguet-commands-inspect-test
    FIREWALL
    ALPM_COMPILE
    CURL
    SOURCES ${_moguet_commands_inspect_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
        MOGUET_ENABLE_TEST_OVERRIDES
        "MOGUET_LOCALE_DIRECTORY=\"${CMAKE_CURRENT_BINARY_DIR}/locale\""
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_commands_inspect_forbidden_sources}
)

set(_moguet_full_cli_test_sources ${MOGUET_PRODUCTION_SOURCES})
list(
    REMOVE_ITEM _moguet_full_cli_test_sources
    source/artifact_archive_metadata.cpp
)
list(
    APPEND _moguet_full_cli_test_sources
    tests/stubs/artifact-identity/archive_metadata_stub.cpp
)

moguet_add_cpp_test(
    moguet-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    CURL
    SOURCES ${_moguet_full_cli_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
        MOGUET_ENABLE_TEST_OVERRIDES
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    moguet-cli-localization-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    CURL
    SOURCES ${_moguet_full_cli_test_sources}
    DEFINITIONS
        "MOGUET_LOCALE_DIRECTORY=\"${CMAKE_CURRENT_BINARY_DIR}/locale\""
        MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
        MOGUET_ENABLE_TEST_OVERRIDES
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    moguet-app-config-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    CURL
    SOURCES ${_moguet_full_cli_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_TEST_CONFIG_PATH
        MOGUET_ENABLE_APP_CONFIG_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

set(
    _moguet_aur_rpc_validation_test_sources
    ${MOGUET_PRODUCTION_SOURCES}
    tests/stubs/package-metadata/alpm_stub.cpp
)
list(
    REMOVE_ITEM _moguet_aur_rpc_validation_test_sources
    source/artifact_archive_metadata.cpp
)
list(
    APPEND _moguet_aur_rpc_validation_test_sources
    tests/stubs/artifact-identity/archive_metadata_stub.cpp
)
moguet_add_cpp_test(
    moguet-aur-rpc-validation-test
    FIREWALL
    ALPM_COMPILE
    CURL
    SOURCES ${_moguet_aur_rpc_validation_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_AUR_RPC_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
)

set(
    _moguet_source_install_characterization_test_sources
    ${MOGUET_PRODUCTION_SOURCES}
)
list(
    REMOVE_ITEM _moguet_source_install_characterization_test_sources
    source/artifact_archive_metadata.cpp
    source/moguet.cpp
)
list(
    APPEND _moguet_source_install_characterization_test_sources
    tests/source_install_characterization.cpp
    tests/stubs/artifact-identity/archive_metadata_stub.cpp
    tests/stubs/package-metadata/alpm_stub.cpp
)
_moguet_test_production_complement(
    _moguet_source_install_characterization_forbidden_sources
    ${_moguet_source_install_characterization_test_sources}
)
moguet_add_cpp_test(
    moguet-source-install-characterization-test
    FIREWALL
    ALPM_COMPILE
    CURL
    SOURCES ${_moguet_source_install_characterization_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
        MOGUET_ENABLE_TEST_OVERRIDES
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
    FORBIDDEN_SOURCES
        ${_moguet_source_install_characterization_forbidden_sources}
)

set(
    _moguet_upgrade_baseline_metadata_test_sources
    ${MOGUET_PRODUCTION_SOURCES}
    tests/stubs/package-metadata/alpm_stub.cpp
)
list(
    REMOVE_ITEM _moguet_upgrade_baseline_metadata_test_sources
    source/artifact_archive_metadata.cpp
)
list(
    APPEND _moguet_upgrade_baseline_metadata_test_sources
    tests/stubs/artifact-identity/archive_metadata_stub.cpp
)
moguet_add_cpp_test(
    moguet-upgrade-baseline-metadata-test
    FIREWALL
    ALPM_COMPILE
    CURL
    SOURCES ${_moguet_upgrade_baseline_metadata_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_TEST_CONFIG_PATH
        MOGUET_ENABLE_APP_CONFIG_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
)

moguet_add_cpp_test(
    application-identity-test
    SOURCES tests/application_identity_test.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    interactive-confirmation-test
    SOURCES
        tests/interactive_confirmation_test.cpp
        source/interactive_confirmation.cpp
        source/logging.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

set(
    _moguet_localization_test_sources
    tests/localization_test.cpp
    source/reviewed_source_production_failure.cpp
    source/reviewed_source_production_outcome.cpp
    source/source_package_identity.cpp
    source/package_identifier.cpp
    source/localization.cpp
)
moguet_add_cpp_test(
    localization-test
    SOURCES ${_moguet_localization_test_sources}
    DEFINITIONS
        "MOGUET_LOCALE_DIRECTORY=\"${CMAKE_CURRENT_BINARY_DIR}/locale\""
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)
moguet_add_cpp_test(
    localization-missing-catalog-test
    SOURCES ${_moguet_localization_test_sources}
    DEFINITIONS
        "MOGUET_LOCALE_DIRECTORY=\"${CMAKE_CURRENT_BINARY_DIR}/tests/missing-locale\""
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    xdg-paths-test
    SOURCES
        tests/xdg_paths_test.cpp
        source/xdg_paths.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    xdg-directory-safety-test
    SOURCES
        tests/xdg_directory_safety_test.cpp
        source/xdg_directory_safety.cpp
        source/xdg_paths.cpp
    DEFINITIONS MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    xdg-generation-store-test
    SOURCES
        tests/xdg_generation_store_test.cpp
        source/xdg_generation_store.cpp
        source/xdg_generation_store_sha256.cpp
        source/xdg_directory_safety.cpp
        source/xdg_paths.cpp
    DEFINITIONS MOGUET_ENABLE_XDG_GENERATION_STORE_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    xdg-state-log-test
    SOURCES
        tests/xdg_state_log_test.cpp
        source/xdg_state_log.cpp
        source/xdg_directory_safety.cpp
        source/xdg_paths.cpp
        source/logging.cpp
    DEFINITIONS MOGUET_TEST_XDG_STATE_LOG_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

set(
    _moguet_trusted_cache_test_sources
    tests/trusted_cache_test.cpp
    source/trusted_cache.cpp
    source/xdg_directory_safety.cpp
    source/xdg_paths.cpp
    source/logging.cpp
)
moguet_add_cpp_test(
    trusted-cache-test
    SOURCES ${_moguet_trusted_cache_test_sources}
    DEFINITIONS MOGUET_ENABLE_TRUSTED_CACHE_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    moguet-root-execution-identity-test
    REAL_ALPM
    CURL
    SOURCES tests/stubs/runtime-identity/geteuid_stub.cpp
    OBJECTS
        $<TARGET_OBJECTS:moguet_production_objects>
        $<TARGET_OBJECTS:moguet_unified_plan_projection_object>
        $<TARGET_OBJECTS:moguet_unified_plan_renderer_object>
    OBJECT_PRODUCTION_SOURCES ${MOGUET_PRODUCTION_SOURCES}
    LINK_OPTIONS LINKER:--wrap=geteuid
)

moguet_add_cpp_test(
    aur-rpc-envelope-validation-test
    ALPM_COMPILE
    CURL
    SOURCES
        tests/aur_rpc_validation_test.cpp
        source/aur_rpc.cpp
        source/aur_constraint_metadata.cpp
        source/package_relation.cpp
        source/dependency_constraint.cpp
        source/dependency_spec.cpp
        source/package_identifier.cpp
        source/logging.cpp
        tests/stubs/package-metadata/alpm_stub.cpp
    DEFINITIONS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_AUR_RPC_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
)

moguet_add_cpp_test(
    app-config-test
    SOURCES
        tests/app_config_test.cpp
        source/app_config.cpp
        source/provider_selection.cpp
        source/dependency_spec.cpp
        source/localization.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    provider-selection-test
    ALPM_COMPILE
    SOURCES
        tests/provider_selection_test.cpp
        source/provider_selection.cpp
        source/dependency_constraint.cpp
        source/provider_installed_state_presentation.cpp
        source/provider_installed_state.cpp
        source/package_metadata.cpp
        source/package_identifier.cpp
        source/shell_words.cpp
        source/dependency_spec.cpp
        source/localization.cpp
        tests/stubs/package-metadata/alpm_stub.cpp
        tests/stubs/package-metadata/process_stub.cpp
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
)

set(
    _moguet_root_package_candidate_test_sources
    tests/root_package_candidate_test.cpp
    source/root_package_candidate.cpp
    source/package_identifier.cpp
)
_moguet_test_production_complement(
    _moguet_root_package_candidate_forbidden_sources
    ${_moguet_root_package_candidate_test_sources}
)
moguet_add_cpp_test(
    root-package-candidate-test
    FIREWALL
    SOURCES ${_moguet_root_package_candidate_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_root_package_candidate_forbidden_sources}
)

set(
    _moguet_root_package_search_test_sources
    tests/root_package_search_test.cpp
    source/root_package_search.cpp
    source/root_package_candidate.cpp
    source/package_identifier.cpp
    tests/stubs/root-package-search/search_stub.cpp
)
_moguet_test_production_complement(
    _moguet_root_package_search_forbidden_sources
    ${_moguet_root_package_search_test_sources}
)
moguet_add_cpp_test(
    root-package-search-test
    FIREWALL
    SOURCES ${_moguet_root_package_search_test_sources}
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_support_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_root_package_search_forbidden_sources}
)

set(
    _moguet_root_package_selection_test_sources
    tests/root_package_selection_test.cpp
    source/root_package_selection.cpp
    source/root_package_candidate.cpp
    source/package_identifier.cpp
)
_moguet_test_production_complement(
    _moguet_root_package_selection_forbidden_sources
    ${_moguet_root_package_selection_test_sources}
)
moguet_add_cpp_test(
    root-package-selection-test
    FIREWALL
    SOURCES ${_moguet_root_package_selection_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_root_package_selection_forbidden_sources}
)

set(
    _moguet_root_package_route_projection_test_sources
    tests/root_package_route_projection_test.cpp
    source/root_package_route_projection.cpp
    source/root_package_selection.cpp
    source/root_package_candidate.cpp
    source/package_identifier.cpp
)
_moguet_test_production_complement(
    _moguet_root_package_route_projection_forbidden_sources
    ${_moguet_root_package_route_projection_test_sources}
)
moguet_add_cpp_test(
    root-package-route-projection-test
    FIREWALL
    SOURCES ${_moguet_root_package_route_projection_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES
        ${_moguet_root_package_route_projection_forbidden_sources}
)

set(
    _moguet_local_package_metadata_test_sources
    tests/local_package_metadata_test.cpp
    source/local_package_metadata.cpp
)
_moguet_test_production_complement(
    _moguet_local_package_metadata_forbidden_sources
    ${_moguet_local_package_metadata_test_sources}
)
moguet_add_cpp_test(
    local-package-metadata-test
    FIREWALL
    SOURCES ${_moguet_local_package_metadata_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_local_package_metadata_forbidden_sources}
)

set(
    _moguet_local_source_root_test_sources
    tests/local_source_root_test.cpp
    source/local_source_root.cpp
    source/local_package_metadata.cpp
)
_moguet_test_production_complement(
    _moguet_local_source_root_forbidden_sources
    ${_moguet_local_source_root_test_sources}
)
moguet_add_cpp_test(
    local-source-root-test
    FIREWALL
    SOURCES ${_moguet_local_source_root_test_sources}
    DEFINITIONS MOGUET_ENABLE_LOCAL_SOURCE_ROOT_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_local_source_root_forbidden_sources}
)

set(
    _moguet_local_dependency_plan_projection_test_sources
    tests/local_dependency_plan_projection_test.cpp
    source/local_dependency_plan_projection.cpp
    source/aur_constraint_metadata.cpp
    source/package_relation.cpp
    source/package_relation_observation.cpp
    source/package_relation_observation_adapter.cpp
    source/dependency_constraint.cpp
    source/dependency_constraint_presentation.cpp
    source/dependency_plan.cpp
    source/dependency_plan_model.cpp
    source/package_relation_presentation.cpp
    source/dependency_spec.cpp
    source/package_identifier.cpp
    source/logging.cpp
    tests/stubs/build-plan-relation-assessment/assessment_stub.cpp
    tests/stubs/local-dependency-plan/aur_rpc_stub.cpp
    tests/stubs/local-dependency-plan/repository_query_stub.cpp
)
_moguet_test_production_complement(
    _moguet_local_dependency_plan_projection_forbidden_sources
    ${_moguet_local_dependency_plan_projection_test_sources}
)
moguet_add_cpp_test(
    local-dependency-plan-projection-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_local_dependency_plan_projection_test_sources}
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_support_include_dir}"
    FORBIDDEN_SOURCES
        ${_moguet_local_dependency_plan_projection_forbidden_sources}
)

set(
    _moguet_local_source_workspace_test_sources
    tests/local_source_workspace_test.cpp
    source/local_source_workspace.cpp
    source/local_source_root.cpp
    source/local_package_metadata.cpp
    source/trusted_cache.cpp
    source/xdg_directory_safety.cpp
    source/xdg_paths.cpp
    source/logging.cpp
)
_moguet_test_production_complement(
    _moguet_local_source_workspace_forbidden_sources
    ${_moguet_local_source_workspace_test_sources}
)
moguet_add_cpp_test(
    local-source-workspace-test
    FIREWALL
    SOURCES ${_moguet_local_source_workspace_test_sources}
    DEFINITIONS MOGUET_ENABLE_LOCAL_SOURCE_WORKSPACE_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_support_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_local_source_workspace_forbidden_sources}
)

set(
    _moguet_local_source_build_test_sources
    tests/local_source_build_test.cpp
    source/local_source_build.cpp
    source/local_source_workspace.cpp
    source/local_source_root.cpp
    source/local_package_metadata.cpp
    source/local_dependency_plan_projection.cpp
    source/aur_constraint_metadata.cpp
    source/package_relation.cpp
    source/package_relation_observation.cpp
    source/package_relation_observation_adapter.cpp
    source/dependency_constraint.cpp
    source/dependency_constraint_presentation.cpp
    source/dependency_plan.cpp
    source/dependency_plan_model.cpp
    source/package_relation_presentation.cpp
    source/dependency_spec.cpp
    source/build_plan_artifact_target_projection.cpp
    source/artifact_workspace.cpp
    source/artifact_identity.cpp
    source/artifact_identity_set.cpp
    source/artifact_identity_selection.cpp
    source/artifact_install_plan.cpp
    source/trusted_cache.cpp
    source/xdg_directory_safety.cpp
    source/xdg_paths.cpp
    source/source_environment.cpp
    source/package_identifier.cpp
    source/shell_words.cpp
    source/logging.cpp
    tests/stubs/build-plan-relation-assessment/assessment_stub.cpp
    tests/stubs/local-dependency-plan/aur_rpc_stub.cpp
    tests/stubs/local-dependency-plan/repository_query_stub.cpp
    tests/stubs/artifact-identity/archive_metadata_stub.cpp
    tests/stubs/local-source-build/process_stub.cpp
)
_moguet_test_production_complement(
    _moguet_local_source_build_forbidden_sources
    ${_moguet_local_source_build_test_sources}
)
moguet_add_cpp_test(
    local-source-build-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_local_source_build_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
        MOGUET_ENABLE_ARTIFACT_WORKSPACE_TEST_HOOKS
        MOGUET_ENABLE_LOCAL_SOURCE_WORKSPACE_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_support_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_local_source_build_forbidden_sources}
)

moguet_add_cpp_test(
    user-config-test
    SOURCES
        tests/user_config_test.cpp
        source/user_config.cpp
        source/cli_parser.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    package-identifier-test
    SOURCES
        tests/package_identifier_test.cpp
        source/package_identifier.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    source-package-identity-test
    SOURCES
        tests/source_package_identity_test.cpp
        tests/vcs_source_identity_test.cpp
        tests/source_entry_parser_test.cpp
        tests/srcinfo_source_metadata_test.cpp
        source/source_package_identity.cpp
        source/vcs_source_identity.cpp
        source/source_entry_parser.cpp
        source/srcinfo_source_metadata.cpp
        source/package_identifier.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    exact-artifact-transaction-protocol-test
    ALPM_COMPILE
    REAL_ALPM
    SOURCES
        tests/exact_artifact_transaction_protocol_test.cpp
        source/exact_artifact_transaction_protocol.cpp
        source/installed_package_record_observation.cpp
        source/source_artifact_install_trusted_protocol.cpp
        source/trusted_alpm_receipt_protocol.cpp
        source/xdg_generation_store_sha256.cpp
        source/package_identifier.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    installed-artifact-binding-test
    SOURCES
        tests/installed_artifact_binding_test.cpp
        source/installed_artifact_binding.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
    DEFINITIONS MOGUET_ENABLE_INSTALLED_ARTIFACT_BINDING_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    installed-package-record-observation-test
    ALPM_COMPILE
    REAL_ALPM
    SOURCES
        tests/installed_package_record_observation_test.cpp
        source/installed_package_record_observation.cpp
        source/source_artifact_install_trusted_protocol.cpp
        source/trusted_alpm_receipt_protocol.cpp
        source/xdg_generation_store_sha256.cpp
        source/package_metadata.cpp
        source/package_identifier.cpp
        source/process.cpp
        source/logging.cpp
        source/shell_words.cpp
    DEFINITIONS MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    devel-build-provenance-test
    SOURCES
        tests/devel_build_provenance_test.cpp
        source/devel_build_provenance.cpp
        source/installed_artifact_binding.cpp
        source/vcs_source_identity.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
    DEFINITIONS
        MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_TEST_HOOKS
        MOGUET_ENABLE_INSTALLED_ARTIFACT_BINDING_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    devel-build-provenance-store-test
    SOURCES
        tests/devel_build_provenance_store_test.cpp
        source/devel_build_provenance_store.cpp
        source/devel_build_provenance_codec.cpp
        source/devel_build_provenance.cpp
        source/installed_artifact_binding.cpp
        source/vcs_source_identity.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
        source/xdg_generation_store.cpp
        source/xdg_generation_store_sha256.cpp
        source/xdg_directory_safety.cpp
        source/xdg_paths.cpp
    DEFINITIONS
        MOGUET_ENABLE_XDG_GENERATION_STORE_TEST_HOOKS
        MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_TEST_HOOKS
        MOGUET_ENABLE_INSTALLED_ARTIFACT_BINDING_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    git-remote-revision-observer-test
    CURL
    SOURCES
        tests/git_remote_revision_observer_test.cpp
        source/git_remote_revision_observer.cpp
        source/vcs_source_identity.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
        source/process.cpp
        source/logging.cpp
        source/trusted_git_process_policy.cpp
    DEFINITIONS MOGUET_ENABLE_GIT_REMOTE_REVISION_OBSERVER_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

set(
    _moguet_source_package_identity_projection_test_sources
    tests/source_package_identity_projection_test.cpp
    source/local_source_build.cpp
    source/local_source_workspace.cpp
    source/local_source_root.cpp
    source/local_package_metadata.cpp
    source/local_dependency_plan_projection.cpp
    source/aur_constraint_metadata.cpp
    source/package_relation.cpp
    source/package_relation_observation.cpp
    source/package_relation_observation_adapter.cpp
    source/dependency_constraint.cpp
    source/dependency_constraint_presentation.cpp
    source/dependency_plan.cpp
    source/dependency_plan_model.cpp
    source/package_relation_presentation.cpp
    source/dependency_spec.cpp
    source/build_plan_artifact_target_projection.cpp
    source/artifact_workspace.cpp
    source/artifact_identity.cpp
    source/artifact_identity_set.cpp
    source/artifact_identity_selection.cpp
    source/artifact_install_plan.cpp
    source/trusted_cache.cpp
    source/xdg_directory_safety.cpp
    source/xdg_paths.cpp
    source/source_environment.cpp
    source/package_identifier.cpp
    source/shell_words.cpp
    source/logging.cpp
    source/source_package_compatibility.cpp
    source/source_package_identity.cpp
    source/source_package_identity_projection.cpp
    tests/stubs/build-plan-relation-assessment/assessment_stub.cpp
    tests/stubs/local-dependency-plan/aur_rpc_stub.cpp
    tests/stubs/local-dependency-plan/repository_query_stub.cpp
    tests/stubs/artifact-identity/archive_metadata_stub.cpp
    tests/stubs/local-source-build/process_stub.cpp
)
_moguet_test_production_complement(
    _moguet_source_package_identity_projection_forbidden_sources
    ${_moguet_source_package_identity_projection_test_sources}
)
moguet_add_cpp_test(
    source-package-identity-projection-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_source_package_identity_projection_test_sources}
    DEFINITIONS MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_support_include_dir}"
    FORBIDDEN_SOURCES
        ${_moguet_source_package_identity_projection_forbidden_sources}
)

moguet_add_cpp_test(
    source-package-compatibility-test
    SOURCES
        tests/source_package_compatibility_test.cpp
        source/source_package_compatibility.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

# The adapter reuses the dependency projection entry in the broader
# source-package identity TU. Section GC keeps unrelated local/artifact
# projection consumers outside this deterministic focused closure.
moguet_add_cpp_test(
    invocation-owned-cleanup-model-test
    ALPM_COMPILE
    SOURCES
        tests/invocation_owned_cleanup_model_test.cpp
        tests/invocation_owned_cleanup_adapter_test.cpp
        tests/remote_aur_cleanup_candidate_collector_test.cpp
        tests/source_artifact_install_receipt_evidence_test.cpp
        tests/trusted_alpm_receipt_test.cpp
        source/invocation_owned_cleanup_adapter.cpp
        source/remote_aur_cleanup_candidate_collector.cpp
        source/invocation_owned_cleanup_model.cpp
        source/package_metadata.cpp
        source/source_artifact_install_receipt_evidence.cpp
        source/build_plan_artifact_target_projection.cpp
        source/dependency_constraint_presentation.cpp
        source/dependency_plan_model.cpp
        source/package_relation.cpp
        source/package_relation_presentation.cpp
        source/source_package_identity.cpp
        source/source_package_identity_projection.cpp
        source/dependency_constraint.cpp
        source/package_identifier.cpp
        source/trusted_alpm_receipt_helper_state.cpp
        source/trusted_alpm_receipt_protocol.cpp
        source/trusted_alpm_receipt_transport.cpp
        source/logging.cpp
        source/shell_words.cpp
        tests/stubs/package-metadata/alpm_stub.cpp
    DEFINITIONS
        MOGUET_ENABLE_CLEANUP_INVOCATION_SESSION_TEST_HOOKS
        MOGUET_ENABLE_INVOCATION_TRANSACTION_LEDGER_TEST_HOOKS
        MOGUET_ENABLE_REMOTE_AUR_CLEANUP_COLLECTOR_TEST_HOOKS
        MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_RECEIPT_TEST_HOOKS
        MOGUET_ENABLE_TRUSTED_ALPM_RECEIPT_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    COMPILE_OPTIONS -ffunction-sections -fdata-sections
    LINK_OPTIONS LINKER:--gc-sections
)

set(
    _moguet_source_artifact_install_trusted_transport_test_sources
    tests/source_artifact_install_trusted_transport_test.cpp
    source/source_artifact_install_trusted_protocol.cpp
    source/source_artifact_install_trusted_helper_state.cpp
    source/exact_artifact_transaction_protocol.cpp
    source/installed_package_record_observation.cpp
    source/installed_artifact_binding_observer.cpp
    source/installed_artifact_binding.cpp
    source/xdg_generation_store_sha256.cpp
    source/source_artifact_install_trusted_transport.cpp
    source/source_artifact_install_receipt_evidence.cpp
    source/package_base_artifact_install_executor.cpp
    source/package_base_artifact_install_plan.cpp
    source/artifact_install_executor.cpp
    source/artifact_install_plan.cpp
    source/artifact_archive_metadata.cpp
    source/artifact_identity.cpp
    source/artifact_identity_set.cpp
    source/artifact_identity_selection.cpp
    source/artifact_workspace.cpp
    source/package_metadata.cpp
    source/source_package_identity.cpp
    source/source_package_identity_projection.cpp
    source/invocation_owned_cleanup_model.cpp
    source/trusted_alpm_receipt_helper_state.cpp
    source/trusted_alpm_receipt_protocol.cpp
    source/trusted_alpm_receipt_transport.cpp
    source/trusted_cache.cpp
    source/xdg_directory_safety.cpp
    source/xdg_paths.cpp
    source/source_environment.cpp
    source/package_identifier.cpp
    source/shell_words.cpp
    source/logging.cpp
)
moguet_add_cpp_test(
    source-artifact-install-trusted-transport-test
    ALPM_COMPILE
    REAL_ALPM
    SOURCES
        ${_moguet_source_artifact_install_trusted_transport_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
        MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_support_include_dir}"
    COMPILE_OPTIONS -ffunction-sections -fdata-sections
    LINK_OPTIONS LINKER:--gc-sections
)

# Installed-container evidence invokes the production transport with real
# process and libalpm boundaries. It is deliberately not a CTest or ordinary
# all target: only the owner-specific container lane builds and runs it.
set(
    _moguet_source_artifact_install_installed_fixture_sources
    tests/source_artifact_install_installed_fixture.cpp
    source/exact_artifact_transaction_protocol.cpp
    source/installed_package_record_observation.cpp
    source/source_install.cpp
    source/source_install_preparation.cpp
    source/cache_authority.cpp
    source/separated_package_base_source_build.cpp
    source/separated_source_build.cpp
    source/reviewed_source_production_failure.cpp
    source/reviewed_source_production_outcome.cpp
    source/build_plan_artifact_target_projection.cpp
    source/dependency_constraint.cpp
    source/dependency_constraint_presentation.cpp
    source/dependency_plan_model.cpp
    source/invocation_owned_cleanup_adapter.cpp
    source/remote_aur_cleanup_candidate_collector.cpp
    source/package_relation.cpp
    source/package_relation_presentation.cpp
    source/source_artifact_install_trusted_protocol.cpp
    source/source_artifact_install_trusted_transport.cpp
    source/xdg_generation_store_sha256.cpp
    source/source_artifact_install_receipt_evidence.cpp
    source/package_base_artifact_install_executor.cpp
    source/package_base_artifact_install_plan.cpp
    source/artifact_install_executor.cpp
    source/artifact_install_plan.cpp
    source/artifact_archive_metadata.cpp
    source/artifact_identity.cpp
    source/artifact_identity_set.cpp
    source/artifact_identity_selection.cpp
    source/artifact_workspace.cpp
    source/package_metadata.cpp
    source/source_package_identity.cpp
    source/source_package_identity_projection.cpp
    source/invocation_owned_cleanup_model.cpp
    source/trusted_alpm_receipt_protocol.cpp
    source/trusted_alpm_receipt_transport.cpp
    source/trusted_cache.cpp
    source/xdg_directory_safety.cpp
    source/xdg_paths.cpp
    source/source_environment.cpp
    source/package_identifier.cpp
    source/shell_words.cpp
    source/logging.cpp
    source/process.cpp
)
add_executable(
    moguet-source-artifact-install-installed-fixture
    EXCLUDE_FROM_ALL
    ${_moguet_source_artifact_install_installed_fixture_sources}
)
set_target_properties(
    moguet-source-artifact-install-installed-fixture
    PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)
target_link_libraries(
    moguet-source-artifact-install-installed-fixture
    PRIVATE
        moguet_external_cppflags
        moguet_build_contract
        moguet_alpm_compile_contract
        moguet_test_real_alpm_link_contract
)
target_include_directories(
    moguet-source-artifact-install-installed-fixture
    PRIVATE "${_moguet_test_source_include_dir}"
)
target_compile_definitions(
    moguet-source-artifact-install-installed-fixture
    PRIVATE MOGUET_ENABLE_REMOTE_AUR_CLEANUP_RUNNER_TEST_HOOKS
)
target_compile_options(
    moguet-source-artifact-install-installed-fixture
    PRIVATE -ffunction-sections -fdata-sections
)
target_link_options(
    moguet-source-artifact-install-installed-fixture
    PRIVATE LINKER:--gc-sections
)

moguet_add_cpp_test(
    reviewed-source-state-test
    SOURCES
        tests/reviewed_source_state_test.cpp
        source/reviewed_source_state.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    reviewed-source-state-store-test
    SOURCES
        tests/reviewed_source_state_store_test.cpp
        source/reviewed_source_state_store.cpp
        source/xdg_generation_store.cpp
        source/xdg_generation_store_sha256.cpp
        source/reviewed_source_state.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
        source/xdg_directory_safety.cpp
        source/xdg_paths.cpp
    DEFINITIONS MOGUET_ENABLE_REVIEWED_SOURCE_STATE_STORE_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    reviewed-source-lifecycle-test
    SOURCES
        tests/reviewed_source_lifecycle_test.cpp
        source/reviewed_source_lifecycle.cpp
        source/reviewed_source_state.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
    DEFINITIONS MOGUET_ENABLE_REVIEWED_SOURCE_LIFECYCLE_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    reviewed-source-acceptance-test
    SOURCES
        tests/reviewed_source_acceptance_test.cpp
        source/reviewed_source_production_failure.cpp
        source/reviewed_source_acceptance.cpp
        source/reviewed_source_lifecycle.cpp
        source/reviewed_source_trusted_review.cpp
        source/reviewed_source_presentation.cpp
        source/reviewed_source_review.cpp
        source/reviewed_source_patch.cpp
        source/reviewed_source_projection.cpp
        source/reviewed_source_state.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
        source/interactive_confirmation.cpp
        source/logging.cpp
    DEFINITIONS
        MOGUET_ENABLE_REVIEWED_SOURCE_LIFECYCLE_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_PRESENTATION_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_ACCEPTANCE_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    reviewed-source-pinned-build-test
    SOURCES
        tests/reviewed_source_pinned_build_test.cpp
        source/reviewed_source_package_base_lease.cpp
        source/reviewed_source_pinned_build.cpp
        source/devel_build_provenance_reviewed_binding.cpp
        source/devel_build_provenance.cpp
        source/installed_artifact_binding.cpp
        source/vcs_source_identity.cpp
        source/reviewed_source_acceptance.cpp
        source/reviewed_source_lifecycle.cpp
        source/reviewed_source_trusted_review.cpp
        source/reviewed_source_presentation.cpp
        source/reviewed_source_review.cpp
        source/reviewed_source_patch.cpp
        source/reviewed_source_projection.cpp
        source/reviewed_source_git_parser.cpp
        source/reviewed_source_state_store.cpp
        source/xdg_generation_store.cpp
        source/xdg_generation_store_sha256.cpp
        source/reviewed_source_state.cpp
        source/trusted_git.cpp
        source/trusted_git_process_policy.cpp
        source/persistent_checkout.cpp
        source/trusted_cache.cpp
        source/xdg_directory_safety.cpp
        source/xdg_paths.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
        source/interactive_confirmation.cpp
        source/process.cpp
        source/logging.cpp
        source/localization.cpp
    DEFINITIONS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_REVIEWED_SOURCE_PRESENTATION_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_ACCEPTANCE_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_STATE_STORE_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_support_include_dir}"
)

set(
    _moguet_invocation_owned_source_build_context_test_sources
    tests/invocation_owned_source_build_context_test.cpp
    source/invocation_owned_source_build_context.cpp
    source/reviewed_source_package_base_lease.cpp
    source/reviewed_source_pinned_build.cpp
    source/devel_build_provenance_reviewed_binding.cpp
    source/devel_build_provenance.cpp
    source/installed_artifact_binding.cpp
    source/vcs_source_identity.cpp
    source/reviewed_source_acceptance.cpp
    source/reviewed_source_lifecycle.cpp
    source/reviewed_source_trusted_review.cpp
    source/reviewed_source_presentation.cpp
    source/reviewed_source_review.cpp
    source/reviewed_source_patch.cpp
    source/reviewed_source_projection.cpp
    source/reviewed_source_git_parser.cpp
    source/reviewed_source_state_store.cpp
    source/xdg_generation_store.cpp
    source/xdg_generation_store_sha256.cpp
    source/reviewed_source_state.cpp
    source/trusted_git.cpp
    source/trusted_git_process_policy.cpp
    source/persistent_checkout.cpp
    source/trusted_cache.cpp
    source/xdg_directory_safety.cpp
    source/xdg_paths.cpp
    source/source_package_identity.cpp
    source/package_identifier.cpp
    source/interactive_confirmation.cpp
    source/process.cpp
    source/logging.cpp
    source/localization.cpp
)
set(
    _moguet_invocation_owned_source_build_context_forbidden_sources
    ${MOGUET_PRODUCTION_SOURCES}
)
list(
    REMOVE_ITEM
    _moguet_invocation_owned_source_build_context_forbidden_sources
    ${_moguet_invocation_owned_source_build_context_test_sources}
)
moguet_add_cpp_test(
    invocation-owned-source-build-context-test
    SOURCES ${_moguet_invocation_owned_source_build_context_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_REVIEWED_SOURCE_PRESENTATION_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_ACCEPTANCE_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_STATE_STORE_TEST_HOOKS
        MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_support_include_dir}"
    FORBIDDEN_SOURCES
        ${_moguet_invocation_owned_source_build_context_forbidden_sources}
)

set(
    _moguet_evaluated_devel_source_build_test_sources
    tests/evaluated_devel_source_build_test.cpp
    source/evaluated_devel_source_build.cpp
    source/artifact_archive_metadata.cpp
    source/artifact_workspace.cpp
    source/invocation_owned_source_build_context.cpp
    source/reviewed_source_package_base_lease.cpp
    source/reviewed_source_pinned_build.cpp
    source/devel_build_provenance_reviewed_binding.cpp
    source/devel_build_provenance.cpp
    source/installed_artifact_binding.cpp
    source/vcs_source_identity.cpp
    source/reviewed_source_acceptance.cpp
    source/reviewed_source_lifecycle.cpp
    source/reviewed_source_trusted_review.cpp
    source/reviewed_source_presentation.cpp
    source/reviewed_source_review.cpp
    source/reviewed_source_patch.cpp
    source/reviewed_source_projection.cpp
    source/reviewed_source_git_parser.cpp
    source/reviewed_source_state_store.cpp
    source/xdg_generation_store.cpp
    source/xdg_generation_store_sha256.cpp
    source/reviewed_source_state.cpp
    source/trusted_git.cpp
    source/trusted_git_process_policy.cpp
    source/git_remote_revision_observer.cpp
    source/persistent_checkout.cpp
    source/trusted_cache.cpp
    source/xdg_directory_safety.cpp
    source/xdg_paths.cpp
    source/source_package_identity.cpp
    source/package_identifier.cpp
    source/local_package_metadata.cpp
    source/srcinfo_source_metadata.cpp
    source/source_entry_parser.cpp
    source/source_environment.cpp
    source/shell_words.cpp
    source/interactive_confirmation.cpp
    source/process.cpp
    source/logging.cpp
    source/localization.cpp
)
set(
    _moguet_evaluated_devel_source_build_forbidden_sources
    ${MOGUET_PRODUCTION_SOURCES}
)
list(
    REMOVE_ITEM
    _moguet_evaluated_devel_source_build_forbidden_sources
    ${_moguet_evaluated_devel_source_build_test_sources}
)
moguet_add_cpp_test(
    evaluated-devel-source-build-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    CURL
    SOURCES ${_moguet_evaluated_devel_source_build_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_REVIEWED_SOURCE_PRESENTATION_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_ACCEPTANCE_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_STATE_STORE_TEST_HOOKS
        MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
        MOGUET_ENABLE_EVALUATED_DEVEL_SOURCE_BUILD_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_support_include_dir}"
    FORBIDDEN_SOURCES
        ${_moguet_evaluated_devel_source_build_forbidden_sources}
)

# S5-A reuses the actual Slice 4 build fixture and the existing transport
# implementation. Only privileged transport processes are intercepted; the
# original Slice 4 target keeps its independent no-transport link firewall.
set(_moguet_evaluated_transport_test_sources
    ${_moguet_evaluated_devel_source_build_test_sources}
    ${_moguet_source_artifact_install_trusted_transport_test_sources})
list(REMOVE_ITEM _moguet_evaluated_transport_test_sources
    tests/source_artifact_install_trusted_transport_test.cpp)
list(REMOVE_DUPLICATES _moguet_evaluated_transport_test_sources)
moguet_add_cpp_test(
    evaluated-devel-source-artifact-transport-test
    ALPM_COMPILE REAL_ALPM CURL
    SOURCES ${_moguet_evaluated_transport_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_REVIEWED_SOURCE_PRESENTATION_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_ACCEPTANCE_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_STATE_STORE_TEST_HOOKS
        MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
        MOGUET_ENABLE_EVALUATED_DEVEL_SOURCE_BUILD_TEST_HOOKS
        MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
        MOGUET_TEST_EVALUATED_DEVEL_ARTIFACT_TRANSPORT
        MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
        MOGUET_TEST_EXACT_INSTALLED_BINDING
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_support_include_dir}"
    COMPILE_OPTIONS -ffunction-sections -fdata-sections
    LINK_OPTIONS LINKER:--gc-sections
)

moguet_add_cpp_test(
    reviewed-source-production-connection-test
    SOURCES
        tests/reviewed_source_production_connection_test.cpp
        source/source_build.cpp
        source/reviewed_source_production_failure.cpp
        source/reviewed_source_production_outcome.cpp
        source/cache_authority.cpp
        source/artifact_workspace.cpp
        source/reviewed_source_package_base_lease.cpp
        source/reviewed_source_pinned_build.cpp
        source/reviewed_source_acceptance.cpp
        source/reviewed_source_lifecycle.cpp
        source/reviewed_source_trusted_review.cpp
        source/reviewed_source_presentation.cpp
        source/reviewed_source_review.cpp
        source/reviewed_source_patch.cpp
        source/reviewed_source_projection.cpp
        source/reviewed_source_git_parser.cpp
        source/reviewed_source_state_store.cpp
        source/xdg_generation_store.cpp
        source/xdg_generation_store_sha256.cpp
        source/reviewed_source_state.cpp
        source/trusted_git.cpp
        source/trusted_git_process_policy.cpp
        source/persistent_checkout.cpp
        source/trusted_cache.cpp
        source/xdg_directory_safety.cpp
        source/xdg_paths.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
        source/interactive_confirmation.cpp
        source/process.cpp
        source/source_environment.cpp
        source/shell_words.cpp
        source/diagnostic_projection.cpp
        source/runtime_diagnostic.cpp
        source/logging.cpp
        source/localization.cpp
        tests/stubs/reviewed-source-production/execution_stub.cpp
    DEFINITIONS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_REVIEWED_SOURCE_PRODUCTION_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_STATE_STORE_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_support_include_dir}"
)

moguet_add_cpp_test(
    reviewed-source-projection-test
    SOURCES
        tests/reviewed_source_projection_test.cpp
        source/reviewed_source_projection.cpp
        source/reviewed_source_git_parser.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    reviewed-source-review-test
    SOURCES
        tests/reviewed_source_review_test.cpp
        source/reviewed_source_review.cpp
        source/reviewed_source_patch.cpp
        source/reviewed_source_projection.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    reviewed-source-patch-test
    SOURCES
        tests/reviewed_source_patch_test.cpp
        source/reviewed_source_patch.cpp
        source/reviewed_source_projection.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    reviewed-source-presentation-test
    SOURCES
        tests/reviewed_source_presentation_test.cpp
        source/reviewed_source_presentation.cpp
        source/reviewed_source_review.cpp
        source/reviewed_source_patch.cpp
        source/reviewed_source_projection.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
    DEFINITIONS MOGUET_ENABLE_REVIEWED_SOURCE_PRESENTATION_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    reviewed-source-git-test
    SOURCES
        tests/reviewed_source_git_test.cpp
        source/reviewed_source_trusted_review.cpp
        source/reviewed_source_lifecycle.cpp
        source/reviewed_source_presentation.cpp
        source/reviewed_source_review.cpp
        source/reviewed_source_patch.cpp
        source/reviewed_source_projection.cpp
        source/reviewed_source_git_parser.cpp
        source/reviewed_source_package_base_lease.cpp
        source/trusted_git.cpp
        source/trusted_git_process_policy.cpp
        source/persistent_checkout.cpp
        source/trusted_cache.cpp
        source/xdg_directory_safety.cpp
        source/xdg_paths.cpp
        source/source_package_identity.cpp
        source/reviewed_source_state.cpp
        source/package_identifier.cpp
        source/process.cpp
        source/logging.cpp
        source/localization.cpp
    DEFINITIONS
        MOGUET_ENABLE_REVIEWED_SOURCE_LIFECYCLE_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_GIT_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_support_include_dir}"
)

moguet_add_cpp_test(
    shell-words-test
    SOURCES
        tests/shell_words_test.cpp
        source/shell_words.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    source-environment-test
    SOURCES
        tests/source_environment_test.cpp
        source/source_environment.cpp
        source/source_preference.cpp
        source/xdg_directory_safety.cpp
        source/xdg_paths.cpp
        source/package_identifier.cpp
        source/shell_words.cpp
    DEFINITIONS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

set(
    _moguet_artifact_workspace_production_sources
    source/artifact_workspace.cpp
    source/trusted_cache.cpp
    source/xdg_directory_safety.cpp
    source/xdg_paths.cpp
    source/source_environment.cpp
    source/package_identifier.cpp
    source/shell_words.cpp
    source/process.cpp
    source/logging.cpp
)
moguet_add_cpp_test(
    artifact-workspace-test
    SOURCES
        tests/artifact_workspace_test.cpp
        ${_moguet_artifact_workspace_production_sources}
    DEFINITIONS
        MOGUET_ENABLE_ARTIFACT_WORKSPACE_TEST_HOOKS
        MOGUET_ENABLE_TRUSTED_CACHE_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

set(
    _moguet_multiple_artifact_workspace_test_sources
    tests/multiple_artifact_workspace_test.cpp
    ${_moguet_artifact_workspace_production_sources}
)
_moguet_test_production_complement(
    _moguet_multiple_artifact_workspace_forbidden_sources
    ${_moguet_multiple_artifact_workspace_test_sources}
)
moguet_add_cpp_test(
    multiple-artifact-workspace-test
    FIREWALL
    SOURCES ${_moguet_multiple_artifact_workspace_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_ARTIFACT_WORKSPACE_TEST_HOOKS
        MOGUET_ENABLE_TRUSTED_CACHE_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES
        ${_moguet_multiple_artifact_workspace_forbidden_sources}
)

set(
    _moguet_makepkg_assignment_precedence_test_sources
    tests/makepkg_assignment_precedence_test.cpp
    source/local_source_metadata_evaluation.cpp
    source/local_source_build.cpp
    source/local_source_root.cpp
    source/local_package_metadata.cpp
    source/artifact_workspace.cpp
    source/trusted_cache.cpp
    source/xdg_directory_safety.cpp
    source/xdg_paths.cpp
    source/source_environment.cpp
    source/package_identifier.cpp
    source/shell_words.cpp
    source/process.cpp
    source/logging.cpp
)
_moguet_test_production_complement(
    _moguet_makepkg_assignment_precedence_forbidden_sources
    ${_moguet_makepkg_assignment_precedence_test_sources}
)
moguet_add_cpp_test(
    makepkg-assignment-precedence-test
    FIREWALL
    SOURCES ${_moguet_makepkg_assignment_precedence_test_sources}
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_support_include_dir}"
    COMPILE_OPTIONS -ffunction-sections -fdata-sections
    LINK_OPTIONS LINKER:--gc-sections
    FORBIDDEN_SOURCES
        ${_moguet_makepkg_assignment_precedence_forbidden_sources}
)

set(
    _moguet_artifact_identity_production_sources
    source/artifact_identity.cpp
    source/artifact_identity_set.cpp
    source/artifact_workspace.cpp
    source/trusted_cache.cpp
    source/xdg_directory_safety.cpp
    source/xdg_paths.cpp
    source/source_environment.cpp
    source/package_identifier.cpp
    source/shell_words.cpp
    source/logging.cpp
)
moguet_add_cpp_test(
    artifact-identity-test
    ALPM_COMPILE
    REAL_ALPM
    SOURCES
        tests/artifact_identity_test.cpp
        ${_moguet_artifact_identity_production_sources}
        source/artifact_archive_metadata.cpp
        tests/stubs/artifact-identity/archive_metadata_stub.cpp
        tests/stubs/artifact-identity/process_stub.cpp
    DEFINITIONS
        MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
        MOGUET_ENABLE_REAL_RETAINED_DESCRIPTOR_ARCHIVE_QUERY
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

set(
    _moguet_multiple_artifact_identity_test_sources
    tests/multiple_artifact_identity_test.cpp
    ${_moguet_artifact_identity_production_sources}
    tests/stubs/artifact-identity/archive_metadata_stub.cpp
    tests/stubs/artifact-identity/process_stub.cpp
)
_moguet_test_production_complement(
    _moguet_multiple_artifact_identity_forbidden_sources
    ${_moguet_multiple_artifact_identity_test_sources}
)
moguet_add_cpp_test(
    multiple-artifact-identity-test
    FIREWALL
    SOURCES ${_moguet_multiple_artifact_identity_test_sources}
    DEFINITIONS MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_multiple_artifact_identity_forbidden_sources}
)

set(
    _moguet_package_base_artifact_install_plan_test_sources
    tests/package_base_artifact_install_plan_test.cpp
    source/package_base_artifact_install_plan.cpp
    source/artifact_install_plan.cpp
    source/package_identifier.cpp
)
_moguet_test_production_complement(
    _moguet_package_base_artifact_install_plan_forbidden_sources
    ${_moguet_package_base_artifact_install_plan_test_sources}
)
moguet_add_cpp_test(
    package-base-artifact-install-plan-test
    FIREWALL
    SOURCES ${_moguet_package_base_artifact_install_plan_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES
        ${_moguet_package_base_artifact_install_plan_forbidden_sources}
)

moguet_add_cpp_test(
    artifact-install-executor-test
    ALPM_COMPILE
    SOURCES
        tests/artifact_install_executor_test.cpp
        source/artifact_install_executor.cpp
        source/artifact_install_plan.cpp
        source/artifact_identity.cpp
        source/artifact_identity_set.cpp
        source/artifact_workspace.cpp
        source/package_metadata.cpp
        source/trusted_cache.cpp
        source/xdg_directory_safety.cpp
        source/xdg_paths.cpp
        source/source_environment.cpp
        source/package_identifier.cpp
        source/shell_words.cpp
        source/logging.cpp
        tests/stubs/package-metadata/alpm_stub.cpp
        tests/stubs/artifact-identity/archive_metadata_stub.cpp
        tests/stubs/artifact-install-executor/process_stub.cpp
    DEFINITIONS MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
)

set(
    _moguet_package_base_artifact_install_executor_test_sources
    tests/package_base_artifact_install_executor_test.cpp
    source/package_base_artifact_install_executor.cpp
    source/package_base_artifact_install_plan.cpp
    source/artifact_install_executor.cpp
    source/artifact_install_plan.cpp
    source/artifact_identity.cpp
    source/artifact_identity_set.cpp
    source/artifact_identity_selection.cpp
    source/artifact_workspace.cpp
    source/package_metadata.cpp
    source/trusted_cache.cpp
    source/xdg_directory_safety.cpp
    source/xdg_paths.cpp
    source/source_environment.cpp
    source/package_identifier.cpp
    source/shell_words.cpp
    source/logging.cpp
    tests/stubs/package-metadata/alpm_stub.cpp
    tests/stubs/artifact-identity/archive_metadata_stub.cpp
    tests/stubs/artifact-install-executor/process_stub.cpp
)
_moguet_test_production_complement(
    _moguet_package_base_artifact_install_executor_forbidden_sources
    ${_moguet_package_base_artifact_install_executor_test_sources}
)
moguet_add_cpp_test(
    package-base-artifact-install-executor-test
    FIREWALL
    ALPM_COMPILE
    SOURCES ${_moguet_package_base_artifact_install_executor_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
        MOGUET_ENABLE_PACKAGE_BASE_ARTIFACT_INSTALL_PLAN_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
    FORBIDDEN_SOURCES
        ${_moguet_package_base_artifact_install_executor_forbidden_sources}
)

moguet_add_cpp_test(
    separated-source-build-test
    ALPM_COMPILE
    SOURCES
        tests/separated_source_build_test.cpp
        source/separated_source_build.cpp
        source/artifact_install_executor.cpp
        source/artifact_install_plan.cpp
        source/artifact_identity.cpp
        source/artifact_identity_set.cpp
        source/artifact_workspace.cpp
        source/package_metadata.cpp
        source/trusted_cache.cpp
        source/xdg_directory_safety.cpp
        source/xdg_paths.cpp
        source/source_environment.cpp
        source/package_identifier.cpp
        source/shell_words.cpp
        source/logging.cpp
        tests/stubs/package-metadata/alpm_stub.cpp
        tests/stubs/artifact-identity/archive_metadata_stub.cpp
        tests/stubs/artifact-install-executor/process_stub.cpp
    DEFINITIONS
        MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
        MOGUET_ENABLE_SEPARATED_SOURCE_BUILD_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
)

set(
    _moguet_separated_package_base_source_build_test_sources
    tests/separated_package_base_source_build_test.cpp
    source/separated_package_base_source_build.cpp
    source/package_base_artifact_install_executor.cpp
    source/package_base_artifact_install_plan.cpp
    source/artifact_install_executor.cpp
    source/artifact_install_plan.cpp
    source/artifact_identity.cpp
    source/artifact_identity_set.cpp
    source/artifact_identity_selection.cpp
    source/artifact_workspace.cpp
    source/package_metadata.cpp
    source/trusted_cache.cpp
    source/xdg_directory_safety.cpp
    source/xdg_paths.cpp
    source/source_environment.cpp
    source/package_identifier.cpp
    source/shell_words.cpp
    source/logging.cpp
    tests/stubs/package-metadata/alpm_stub.cpp
    tests/stubs/artifact-identity/archive_metadata_stub.cpp
    tests/stubs/artifact-install-executor/process_stub.cpp
)
_moguet_test_production_complement(
    _moguet_separated_package_base_source_build_forbidden_sources
    ${_moguet_separated_package_base_source_build_test_sources}
)
moguet_add_cpp_test(
    separated-package-base-source-build-test
    FIREWALL
    ALPM_COMPILE
    SOURCES ${_moguet_separated_package_base_source_build_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
        MOGUET_ENABLE_SEPARATED_PACKAGE_BASE_SOURCE_BUILD_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
    FORBIDDEN_SOURCES
        ${_moguet_separated_package_base_source_build_forbidden_sources}
)

moguet_add_cpp_test(
    production-source-build-test
    ALPM_COMPILE
    CURL
    SOURCES
        tests/production_source_build_test.cpp
        source/app_config.cpp
        source/aur_constraint_metadata.cpp
        source/package_relation.cpp
        source/package_relation_observation.cpp
        source/package_relation_observation_adapter.cpp
        source/provider_selection.cpp
        source/source_install.cpp
        source/local_source_build_dependency_preparation.cpp
        source/local_dependency_plan_projection.cpp
        source/dependency_constraint.cpp
        source/dependency_constraint_presentation.cpp
        source/local_package_metadata.cpp
        source/cache_authority.cpp
        source/source_install_preparation.cpp
        source/source_build.cpp
        source/reviewed_source_production_failure.cpp
        source/reviewed_source_production_outcome.cpp
        source/reviewed_source_package_base_lease.cpp
        source/reviewed_source_pinned_build.cpp
        source/reviewed_source_acceptance.cpp
        source/reviewed_source_lifecycle.cpp
        source/reviewed_source_trusted_review.cpp
        source/reviewed_source_presentation.cpp
        source/reviewed_source_review.cpp
        source/reviewed_source_patch.cpp
        source/reviewed_source_projection.cpp
        source/reviewed_source_git_parser.cpp
        source/reviewed_source_state_store.cpp
        source/xdg_generation_store.cpp
        source/xdg_generation_store_sha256.cpp
        source/reviewed_source_state.cpp
        source/source_package_identity.cpp
        source/source_package_identity_projection.cpp
        source/interactive_confirmation.cpp
        source/invocation_owned_cleanup_model.cpp
        source/invocation_owned_cleanup_adapter.cpp
        source/remote_aur_cleanup_candidate_collector.cpp
        source/diagnostic_projection.cpp
        source/runtime_diagnostic.cpp
        source/separated_source_build.cpp
        source/separated_package_base_source_build.cpp
        source/package_base_artifact_install_executor.cpp
        source/package_base_artifact_install_plan.cpp
        source/artifact_install_executor.cpp
        source/artifact_install_plan.cpp
        source/artifact_identity.cpp
        source/artifact_identity_set.cpp
        source/artifact_identity_selection.cpp
        source/artifact_workspace.cpp
        source/package_metadata.cpp
        source/trusted_cache.cpp
        source/xdg_directory_safety.cpp
        source/xdg_paths.cpp
        source/persistent_checkout.cpp
        tests/stubs/trusted-git/process_stub.cpp
        source/source_environment.cpp
        source/source_preference.cpp
        source/build_plan_artifact_target_projection.cpp
        source/dependency_plan.cpp
        source/dependency_plan_model.cpp
        source/package_relation_presentation.cpp
        source/dependency_spec.cpp
        source/package_identifier.cpp
        source/trusted_alpm_receipt_protocol.cpp
        tests/stubs/build-plan-relation-assessment/assessment_stub.cpp
        tests/stubs/local-dependency-plan/aur_rpc_stub.cpp
        tests/stubs/local-dependency-plan/repository_query_stub.cpp
        source/shell_words.cpp
        source/logging.cpp
        tests/stubs/package-metadata/alpm_stub.cpp
        tests/stubs/artifact-identity/archive_metadata_stub.cpp
        tests/stubs/artifact-install-executor/process_stub.cpp
    DEFINITIONS
        MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
        MOGUET_ENABLE_SEPARATED_SOURCE_BUILD_TEST_HOOKS
        MOGUET_ENABLE_SEPARATED_PACKAGE_BASE_SOURCE_BUILD_TEST_HOOKS
        MOGUET_ENABLE_CLEANUP_INVOCATION_SESSION_TEST_HOOKS
        MOGUET_ENABLE_REMOTE_AUR_CLEANUP_COLLECTOR_STUB
        MOGUET_ENABLE_REVIEWED_SOURCE_PRODUCTION_TEST_HOOKS
        MOGUET_ENABLE_TEST_OVERRIDES
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_support_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
    COMPILE_OPTIONS -ffunction-sections -fdata-sections
    LINK_OPTIONS LINKER:--gc-sections
)

moguet_add_cpp_test(
    process-capture-test
    SOURCES
        tests/process_capture_test.cpp
        source/process.cpp
        source/shell_words.cpp
        source/logging.cpp
    DEFINITIONS MOGUET_ENABLE_PROCESS_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    bounded-process-test
    SOURCES
        tests/bounded_process_test.cpp
        source/process.cpp
        source/logging.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    makepkg-devel-phase-characterization-test
    SOURCES
        tests/makepkg_devel_phase_characterization_test.cpp
        source/process.cpp
        source/logging.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    process-stdin-fd-test
    SOURCES
        tests/process_stdin_fd_test.cpp
        source/process.cpp
        source/logging.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    aur-update-plan-test
    SOURCES
        tests/aur_update_plan_test.cpp
        tests/devel_package_classification_test.cpp
        tests/devel_update_model_test.cpp
        source/aur_update_plan.cpp
        source/devel_package_classification.cpp
        source/devel_update_model.cpp
        source/vcs_source_identity.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

set(
    _moguet_upgrade_all_plan_test_sources
    tests/test-upgrade-all-plan.cpp
    source/upgrade_all_plan.cpp
)
_moguet_test_production_complement(
    _moguet_upgrade_all_plan_forbidden_sources
    ${_moguet_upgrade_all_plan_test_sources}
)
moguet_add_cpp_test(
    upgrade-all-plan-test
    FIREWALL
    SOURCES ${_moguet_upgrade_all_plan_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_upgrade_all_plan_forbidden_sources}
)

set(
    _moguet_system_source_upgrade_test_sources
    tests/system_source_upgrade_test.cpp
    source/system_source_upgrade.cpp
    source/unified_plan_projection.cpp
    source/unified_plan_observation.cpp
    source/dependency_constraint.cpp
    source/cache_authority.cpp
    source/trusted_cache.cpp
    source/xdg_directory_safety.cpp
    source/xdg_paths.cpp
    source/package_identifier.cpp
    source/shell_words.cpp
    source/logging.cpp
    tests/stubs/system-source-upgrade/phase_stub.cpp
)
_moguet_test_production_complement(
    _moguet_system_source_upgrade_forbidden_sources
    ${_moguet_system_source_upgrade_test_sources}
)
moguet_add_cpp_test(
    system-source-upgrade-test
    FIREWALL
    SOURCES ${_moguet_system_source_upgrade_test_sources}
    DEFINITIONS MOGUET_ENABLE_SYSTEM_SOURCE_UPGRADE_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    COMPILE_OPTIONS -ffunction-sections -fdata-sections
    LINK_OPTIONS LINKER:--gc-sections
    FORBIDDEN_SOURCES ${_moguet_system_source_upgrade_forbidden_sources}
)

moguet_add_cpp_test(
    aur-update-query-test
    SOURCES
        tests/aur_update_query_test.cpp
        source/aur_update_query.cpp
        source/aur_update_plan.cpp
        source/devel_package_classification.cpp
        source/devel_update_model.cpp
        source/vcs_source_identity.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
        source/shell_words.cpp
        source/logging.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

set(
    _moguet_aur_update_execution_preflight_test_sources
    tests/aur_update_execution_preflight_test.cpp
    source/aur_update_plan.cpp
    source/aur_update_execution_preflight.cpp
    source/devel_package_classification.cpp
    source/devel_update_model.cpp
    source/vcs_source_identity.cpp
    source/source_package_identity.cpp
    source/build_plan_artifact_target_projection.cpp
    source/dependency_constraint.cpp
    source/dependency_constraint_presentation.cpp
    source/dependency_plan_model.cpp
    source/package_relation.cpp
    source/package_relation_presentation.cpp
    source/dependency_spec.cpp
    source/package_identifier.cpp
    tests/stubs/aur-update-execution-preflight/preflight_stub.cpp
)
set(
    _moguet_aur_update_execution_preflight_forbidden_sources
    source/build_plan_relation_assessment.cpp
    source/installed_package_relation_inventory.cpp
    source/package_relation_assessment.cpp
    source/package_relation_observation.cpp
    source/package_relation_observation_adapter.cpp
    source/package_constraint_metadata.cpp
    source/package_metadata.cpp
    source/repository_query.cpp
)
moguet_add_cpp_test(
    aur-update-execution-preflight-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_aur_update_execution_preflight_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES
        ${_moguet_aur_update_execution_preflight_forbidden_sources}
)

moguet_add_cpp_test(
    aur-update-execution-preflight-integration-test
    ALPM_COMPILE
    SOURCES
        tests/aur_update_execution_preflight_integration_test.cpp
        source/aur_update_plan.cpp
        source/aur_update_execution_preflight.cpp
        source/devel_package_classification.cpp
        source/devel_update_model.cpp
        source/vcs_source_identity.cpp
        source/source_package_identity.cpp
        source/aur_constraint_metadata.cpp
        source/build_plan_relation_assessment.cpp
        source/installed_package_relation_inventory.cpp
        source/package_relation_assessment.cpp
        source/package_relation.cpp
        source/package_relation_observation.cpp
        source/package_relation_observation_adapter.cpp
        source/build_plan_artifact_target_projection.cpp
        source/dependency_constraint.cpp
        source/dependency_constraint_presentation.cpp
        source/dependency_plan.cpp
        source/dependency_plan_model.cpp
        source/package_relation_presentation.cpp
        source/dependency_spec.cpp
        source/package_constraint_metadata.cpp
        source/repository_query.cpp
        source/package_metadata.cpp
        source/package_identifier.cpp
        source/shell_words.cpp
        tests/stubs/package-metadata/alpm_stub.cpp
        tests/stubs/aur-update-execution-preflight-integration/integration_stub.cpp
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/stubs/aur-update-execution-preflight-integration"
)

moguet_add_cpp_test(
    aur-update-execution-preparation-test
    ALPM_COMPILE
    REAL_ALPM
    SOURCES
        tests/aur_update_execution_preparation_test.cpp
        source/devel_package_classification.cpp
        source/vcs_source_identity.cpp
        source/aur_update_execution_preparation.cpp
        source/build_plan_artifact_target_projection.cpp
        source/dependency_constraint.cpp
        source/dependency_constraint_presentation.cpp
        source/dependency_plan_model.cpp
        source/package_relation.cpp
        source/package_relation_presentation.cpp
        source/source_install_preparation.cpp
        source/source_package_identity.cpp
        source/source_environment.cpp
        source/shell_words.cpp
        source/package_identifier.cpp
        tests/stubs/aur-update-execution-preparation/preparation_stub.cpp
    DEFINITIONS MOGUET_ENABLE_AUR_UPDATE_EXECUTION_PREPARATION_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    aur-update-execution-preparation-integration-test
    ALPM_COMPILE
    REAL_ALPM
    SOURCES
        tests/aur_update_execution_preparation_integration_test.cpp
        source/aur_update_execution_preparation.cpp
        source/build_plan_artifact_target_projection.cpp
        source/dependency_constraint.cpp
        source/dependency_constraint_presentation.cpp
        source/dependency_plan_model.cpp
        source/package_relation.cpp
        source/package_relation_presentation.cpp
        source/source_install_preparation.cpp
        source/source_package_identity.cpp
        source/source_preference.cpp
        source/source_environment.cpp
        source/xdg_directory_safety.cpp
        source/xdg_paths.cpp
        source/shell_words.cpp
        source/package_identifier.cpp
        tests/stubs/aur-update-execution-preparation-integration/preparation_stub.cpp
    DEFINITIONS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_AUR_UPDATE_EXECUTION_PREPARATION_TEST_HOOKS
        MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

set(
    _moguet_aur_update_execution_runner_test_sources
    tests/aur_update_execution_runner_test.cpp
    source/aur_update_execution_runner.cpp
    source/aur_update_execution_preparation.cpp
    source/build_plan_artifact_target_projection.cpp
    source/dependency_constraint.cpp
    source/dependency_constraint_presentation.cpp
    source/dependency_plan_model.cpp
    source/package_relation.cpp
    source/package_relation_presentation.cpp
    source/source_install_preparation.cpp
    source/reviewed_source_state.cpp
    source/source_package_identity.cpp
    source/source_environment.cpp
    source/shell_words.cpp
    source/package_identifier.cpp
    tests/stubs/aur-update-execution-preparation/preparation_stub.cpp
    tests/stubs/aur-update-execution-runner/execution_stub.cpp
)
set(
    _moguet_aur_update_execution_runner_forbidden_sources
    source/process.cpp
    source/checkout_fetch.cpp
    source/persistent_checkout.cpp
    source/artifact_workspace.cpp
    source/separated_source_build.cpp
    source/separated_package_base_source_build.cpp
    source/artifact_install_executor.cpp
    source/package_base_artifact_install_executor.cpp
    source/source_build.cpp
    source/source_install.cpp
)
moguet_add_cpp_test(
    aur-update-execution-runner-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_aur_update_execution_runner_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_AUR_UPDATE_EXECUTION_PREPARATION_TEST_HOOKS
        MOGUET_ENABLE_AUR_UPDATE_EXECUTION_RUNNER_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES
        ${_moguet_aur_update_execution_runner_forbidden_sources}
)

set(
    _moguet_aur_update_operation_result_test_sources
    tests/aur_update_operation_result_test.cpp
    source/aur_update_operation_result.cpp
    source/devel_package_classification.cpp
    source/devel_update_model.cpp
    source/vcs_source_identity.cpp
    source/aur_update_execution_preparation.cpp
    source/build_plan_artifact_target_projection.cpp
    source/dependency_constraint.cpp
    source/dependency_constraint_presentation.cpp
    source/dependency_plan_model.cpp
    source/package_relation.cpp
    source/package_relation_presentation.cpp
    source/source_install_preparation.cpp
    source/source_package_identity.cpp
    source/source_environment.cpp
    source/shell_words.cpp
    source/package_identifier.cpp
    tests/stubs/aur-update-execution-preparation/preparation_stub.cpp
)
set(
    _moguet_aur_update_operation_result_forbidden_sources
    ${_moguet_aur_update_execution_runner_forbidden_sources}
)
moguet_add_cpp_test(
    aur-update-operation-result-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_aur_update_operation_result_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_aur_update_operation_result_forbidden_sources}
)

set(
    _moguet_filtered_aur_update_operation_test_sources
    tests/filtered_aur_update_operation_test.cpp
    source/app_config.cpp
    source/provider_selection.cpp
    source/filtered_aur_update_operation.cpp
    source/system_aur_update_operation.cpp
    source/upgrade_all_plan.cpp
    source/aur_update_query.cpp
    source/aur_update_plan.cpp
    source/devel_package_classification.cpp
    source/devel_update_model.cpp
    source/vcs_source_identity.cpp
    source/aur_update_execution_preflight.cpp
    source/aur_update_execution_preparation.cpp
    source/build_plan_artifact_target_projection.cpp
    source/dependency_constraint.cpp
    source/dependency_constraint_presentation.cpp
    source/dependency_plan_model.cpp
    source/package_relation.cpp
    source/package_relation_presentation.cpp
    source/aur_update_execution_runner.cpp
    source/aur_update_operation_result.cpp
    source/source_install_preparation.cpp
    source/source_package_identity.cpp
    source/source_environment.cpp
    source/shell_words.cpp
    source/package_identifier.cpp
    source/dependency_spec.cpp
    source/logging.cpp
    tests/stubs/filtered-aur-update-operation/query_stub.cpp
    tests/stubs/aur-update-execution-preflight/preflight_stub.cpp
    tests/stubs/aur-update-execution-preparation/preparation_stub.cpp
    tests/stubs/aur-update-execution-runner/execution_stub.cpp
)
set(
    _moguet_filtered_aur_update_operation_forbidden_sources
    source/aur_rpc.cpp
    source/package_metadata.cpp
    source/process.cpp
    source/checkout_fetch.cpp
    source/persistent_checkout.cpp
    source/artifact_workspace.cpp
    source/commands_sync.cpp
    source/separated_source_build.cpp
    source/separated_package_base_source_build.cpp
    source/artifact_install_executor.cpp
    source/package_base_artifact_install_executor.cpp
    source/source_build.cpp
    source/source_install.cpp
)
moguet_add_cpp_test(
    filtered-aur-update-operation-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_filtered_aur_update_operation_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_AUR_UPDATE_EXECUTION_PREPARATION_TEST_HOOKS
        MOGUET_ENABLE_AUR_UPDATE_EXECUTION_RUNNER_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES
        ${_moguet_filtered_aur_update_operation_forbidden_sources}
)

set(
    _moguet_upgrade_all_operation_test_sources
    tests/upgrade_all_operation_test.cpp
    source/app_config.cpp
    source/provider_selection.cpp
    source/upgrade_all_operation.cpp
    source/upgrade_all_operation_result.cpp
    source/operation_state_model.cpp
    source/diagnostic_projection.cpp
    source/presentation_projection.cpp
    source/cross_source_version_lock.cpp
    source/cross_source_version_lock_observation.cpp
    source/system_source_upgrade.cpp
    source/unified_plan_projection.cpp
    source/unified_plan_observation.cpp
    source/cache_authority.cpp
    source/trusted_cache.cpp
    source/xdg_directory_safety.cpp
    source/xdg_paths.cpp
    source/filtered_aur_update_operation.cpp
    source/upgrade_all_plan.cpp
    source/aur_update_query.cpp
    source/aur_update_plan.cpp
    source/devel_package_classification.cpp
    source/devel_update_model.cpp
    source/vcs_source_identity.cpp
    source/aur_update_execution_preflight.cpp
    source/aur_update_execution_preparation.cpp
    source/build_plan_artifact_target_projection.cpp
    source/dependency_constraint.cpp
    source/dependency_constraint_presentation.cpp
    source/dependency_plan_model.cpp
    source/package_relation.cpp
    source/package_relation_observation.cpp
    source/package_relation_presentation.cpp
    source/aur_update_execution_runner.cpp
    source/aur_update_operation_result.cpp
    source/source_install_preparation.cpp
    source/source_package_identity.cpp
    source/source_environment.cpp
    source/shell_words.cpp
    source/package_identifier.cpp
    source/dependency_spec.cpp
    source/logging.cpp
    tests/stubs/upgrade-all-operation/operation_stub.cpp
)
_moguet_test_production_complement(
    _moguet_upgrade_all_operation_forbidden_sources
    ${_moguet_upgrade_all_operation_test_sources}
)
moguet_add_cpp_test(
    upgrade-all-operation-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_upgrade_all_operation_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_UPGRADE_ALL_OPERATION_TEST_HOOKS
        MOGUET_ENABLE_SYSTEM_SOURCE_UPGRADE_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    COMPILE_OPTIONS -ffunction-sections -fdata-sections
    LINK_OPTIONS LINKER:--gc-sections
    FORBIDDEN_SOURCES ${_moguet_upgrade_all_operation_forbidden_sources}
)

set(
    _moguet_cli_diagnostic_model_test_sources
    tests/cli_diagnostic_model_test.cpp
    source/dependency_constraint.cpp
    source/dependency_constraint_presentation.cpp
    source/dependency_plan_model.cpp
    source/package_relation_presentation.cpp
    source/package_relation.cpp
    source/diagnostic_projection.cpp
    source/operation_state_model.cpp
    source/presentation_projection.cpp
)
_moguet_test_production_complement(
    _moguet_cli_diagnostic_model_forbidden_sources
    ${_moguet_cli_diagnostic_model_test_sources}
)
moguet_add_cpp_test(
    cli-diagnostic-model-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_cli_diagnostic_model_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_cli_diagnostic_model_forbidden_sources}
)

set(
    _moguet_runtime_cli_connection_test_sources
    tests/runtime_cli_connection_test.cpp
    source/cli_parser.cpp
    source/cli_routing.cpp
    source/cli_runtime_contract.cpp
    source/cli_public_projection.cpp
    source/runtime_diagnostic.cpp
    source/source_environment.cpp
)
_moguet_test_production_complement(
    _moguet_runtime_cli_connection_forbidden_sources
    ${_moguet_runtime_cli_connection_test_sources}
)
moguet_add_cpp_test(
    runtime-cli-connection-test
    FIREWALL
    SOURCES ${_moguet_runtime_cli_connection_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    COMPILE_OPTIONS -ffunction-sections -fdata-sections
    LINK_OPTIONS LINKER:--gc-sections
    FORBIDDEN_SOURCES ${_moguet_runtime_cli_connection_forbidden_sources}
)

set(
    _moguet_dependency_plan_model_test_sources
    tests/dependency_plan_model_test.cpp
    source/dependency_plan.cpp
    source/dependency_plan_model.cpp
    source/package_relation_presentation.cpp
    source/aur_constraint_metadata.cpp
    source/package_relation.cpp
    source/package_relation_observation.cpp
    source/package_relation_observation_adapter.cpp
    source/dependency_constraint.cpp
    source/dependency_constraint_presentation.cpp
    source/dependency_spec.cpp
    source/package_identifier.cpp
    source/logging.cpp
    tests/stubs/build-plan-relation-assessment/assessment_stub.cpp
    tests/stubs/dependency-plan/aur_rpc_stub.cpp
    tests/stubs/dependency-plan/repository_query_stub.cpp
)
_moguet_test_production_complement(
    _moguet_dependency_plan_model_forbidden_sources
    ${_moguet_dependency_plan_model_test_sources}
)
moguet_add_cpp_test(
    dependency-plan-model-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_dependency_plan_model_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_dependency_plan_model_forbidden_sources}
)

set(
    _moguet_build_plan_artifact_target_projection_test_sources
    tests/build_plan_artifact_target_projection_test.cpp
    source/build_plan_artifact_target_projection.cpp
    source/dependency_constraint.cpp
    source/dependency_constraint_presentation.cpp
    source/dependency_plan_model.cpp
    source/package_relation.cpp
    source/package_relation_presentation.cpp
    source/package_identifier.cpp
)
_moguet_test_production_complement(
    _moguet_build_plan_artifact_target_projection_forbidden_sources
    ${_moguet_build_plan_artifact_target_projection_test_sources}
)
moguet_add_cpp_test(
    build-plan-artifact-target-projection-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_build_plan_artifact_target_projection_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES
        ${_moguet_build_plan_artifact_target_projection_forbidden_sources}
)

set(
    _moguet_unified_plan_observation_test_sources
    tests/unified_plan_observation_test.cpp
    source/unified_plan_observation.cpp
    source/package_relation.cpp
    source/dependency_constraint.cpp
)
_moguet_test_production_complement(
    _moguet_unified_plan_observation_forbidden_sources
    ${_moguet_unified_plan_observation_test_sources}
)
moguet_add_cpp_test(
    unified-plan-observation-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_unified_plan_observation_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_unified_plan_observation_forbidden_sources}
)

set(
    _moguet_unified_plan_projection_test_sources
    tests/unified_plan_projection_test.cpp
    source/unified_plan_observation.cpp
    source/build_plan_artifact_target_projection.cpp
    source/root_package_route_projection.cpp
    source/root_package_selection.cpp
    source/root_package_candidate.cpp
    source/local_source_root.cpp
    source/local_package_metadata.cpp
    source/local_dependency_plan_projection.cpp
    source/aur_constraint_metadata.cpp
    source/package_relation.cpp
    source/package_relation_observation.cpp
    source/package_relation_observation_adapter.cpp
    source/dependency_constraint.cpp
    source/dependency_constraint_presentation.cpp
    source/dependency_plan.cpp
    source/dependency_plan_model.cpp
    source/package_relation_presentation.cpp
    source/dependency_spec.cpp
    source/source_package_identity.cpp
    source/package_identifier.cpp
    source/logging.cpp
    tests/stubs/build-plan-relation-assessment/assessment_stub.cpp
    tests/stubs/local-dependency-plan/aur_rpc_stub.cpp
    tests/stubs/local-dependency-plan/repository_query_stub.cpp
)
_moguet_test_production_complement(
    _moguet_unified_plan_projection_forbidden_sources
    ${_moguet_unified_plan_projection_test_sources}
    source/unified_plan_projection.cpp
)
moguet_add_cpp_test(
    unified-plan-projection-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_unified_plan_projection_test_sources}
    OBJECTS $<TARGET_OBJECTS:moguet_unified_plan_projection_object>
    OBJECT_PRODUCTION_SOURCES source/unified_plan_projection.cpp
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_support_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_unified_plan_projection_forbidden_sources}
)

set(
    _moguet_unified_plan_renderer_test_sources
    tests/unified_plan_renderer_test.cpp
    source/unified_plan_observation.cpp
    source/local_dependency_plan_projection.cpp
    source/aur_constraint_metadata.cpp
    source/package_relation.cpp
    source/package_relation_observation.cpp
    source/package_relation_observation_adapter.cpp
    source/dependency_constraint.cpp
    source/dependency_constraint_presentation.cpp
    source/dependency_plan.cpp
    source/dependency_plan_model.cpp
    source/package_relation_presentation.cpp
    source/dependency_spec.cpp
    source/package_identifier.cpp
    source/logging.cpp
    tests/stubs/build-plan-relation-assessment/assessment_stub.cpp
    tests/stubs/local-dependency-plan/aur_rpc_stub.cpp
    tests/stubs/local-dependency-plan/repository_query_stub.cpp
)
_moguet_test_production_complement(
    _moguet_unified_plan_renderer_forbidden_sources
    ${_moguet_unified_plan_renderer_test_sources}
    source/unified_plan_renderer.cpp
)
moguet_add_cpp_test(
    unified-plan-renderer-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_unified_plan_renderer_test_sources}
    OBJECTS $<TARGET_OBJECTS:moguet_unified_plan_renderer_object>
    OBJECT_PRODUCTION_SOURCES source/unified_plan_renderer.cpp
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_support_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_unified_plan_renderer_forbidden_sources}
)

moguet_add_cpp_test(
    repository-query-test
    ALPM_COMPILE
    SOURCES
        tests/repository_query_test.cpp
        source/repository_query.cpp
        source/package_constraint_metadata.cpp
        source/dependency_constraint.cpp
        source/package_metadata.cpp
        source/dependency_spec.cpp
        source/package_identifier.cpp
        source/shell_words.cpp
        tests/stubs/package-metadata/alpm_stub.cpp
        tests/stubs/repository-query/process_stub.cpp
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
)

set(
    _moguet_artifact_install_plan_production_sources
    source/artifact_install_plan.cpp
    source/package_identifier.cpp
)
moguet_add_cpp_test(
    artifact-install-plan-test
    SOURCES
        tests/artifact_install_plan_test.cpp
        ${_moguet_artifact_install_plan_production_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

set(
    _moguet_artifact_selection_model_test_sources
    tests/artifact_selection_model_test.cpp
    ${_moguet_artifact_install_plan_production_sources}
)
_moguet_test_production_complement(
    _moguet_artifact_selection_model_forbidden_sources
    ${_moguet_artifact_selection_model_test_sources}
)
moguet_add_cpp_test(
    artifact-selection-model-test
    FIREWALL
    SOURCES ${_moguet_artifact_selection_model_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_artifact_selection_model_forbidden_sources}
)

set(
    _moguet_artifact_identity_selection_test_sources
    tests/artifact_identity_selection_test.cpp
    source/artifact_identity_selection.cpp
    source/artifact_identity_set.cpp
    source/artifact_install_plan.cpp
    source/package_identifier.cpp
)
_moguet_test_production_complement(
    _moguet_artifact_identity_selection_forbidden_sources
    ${_moguet_artifact_identity_selection_test_sources}
)
moguet_add_cpp_test(
    artifact-identity-selection-test
    FIREWALL
    SOURCES ${_moguet_artifact_identity_selection_test_sources}
    DEFINITIONS MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_artifact_identity_selection_forbidden_sources}
)

moguet_add_cpp_test(
    package-metadata-test
    ALPM_COMPILE
    SOURCES
        tests/package_metadata_test.cpp
        source/package_metadata.cpp
        source/package_identifier.cpp
        source/shell_words.cpp
        tests/stubs/package-metadata/alpm_stub.cpp
        tests/stubs/package-metadata/process_stub.cpp
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
)

set(
    _moguet_provider_installed_state_test_sources
    tests/provider_installed_state_test.cpp
    source/provider_installed_state.cpp
    source/package_metadata.cpp
    source/package_identifier.cpp
    source/shell_words.cpp
    tests/stubs/package-metadata/alpm_stub.cpp
    tests/stubs/package-metadata/process_stub.cpp
)
_moguet_test_production_complement(
    _moguet_provider_installed_state_forbidden_sources
    ${_moguet_provider_installed_state_test_sources}
)
moguet_add_cpp_test(
    provider-installed-state-test
    FIREWALL
    ALPM_COMPILE
    SOURCES ${_moguet_provider_installed_state_test_sources}
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_provider_installed_state_forbidden_sources}
)

set(
    _moguet_dependency_constraint_test_sources
    tests/dependency_constraint_test.cpp
    source/dependency_constraint.cpp
)
_moguet_test_production_complement(
    _moguet_dependency_constraint_forbidden_sources
    ${_moguet_dependency_constraint_test_sources}
)
moguet_add_cpp_test(
    dependency-constraint-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_dependency_constraint_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_dependency_constraint_forbidden_sources}
)

moguet_add_cpp_test(
    cross-source-version-lock-test
    ALPM_COMPILE
    REAL_ALPM
    SOURCES
        tests/cross_source_version_lock_test.cpp
        tests/cross_source_version_lock_observation_test.cpp
        source/cross_source_version_lock.cpp
        source/cross_source_version_lock_observation.cpp
        source/package_relation_observation.cpp
        source/package_relation.cpp
        source/dependency_constraint.cpp
        source/package_identifier.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

set(
    _moguet_package_relation_test_sources
    tests/package_relation_test.cpp
    source/package_relation.cpp
    source/dependency_constraint.cpp
)
_moguet_test_production_complement(
    _moguet_package_relation_forbidden_sources
    ${_moguet_package_relation_test_sources}
)
moguet_add_cpp_test(
    package-relation-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_package_relation_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_package_relation_forbidden_sources}
)

set(
    _moguet_package_relation_observation_test_sources
    tests/package_relation_observation_test.cpp
    source/package_relation_observation.cpp
    source/package_relation.cpp
    source/dependency_constraint.cpp
    source/package_identifier.cpp
)
_moguet_test_production_complement(
    _moguet_package_relation_observation_forbidden_sources
    ${_moguet_package_relation_observation_test_sources}
)
moguet_add_cpp_test(
    package-relation-observation-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_package_relation_observation_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES
        ${_moguet_package_relation_observation_forbidden_sources}
)

set(
    _moguet_package_relation_assessment_test_sources
    tests/package_relation_assessment_test.cpp
    source/package_relation_assessment.cpp
    source/package_relation_observation.cpp
    source/package_relation.cpp
    source/dependency_constraint.cpp
    source/package_identifier.cpp
)
_moguet_test_production_complement(
    _moguet_package_relation_assessment_forbidden_sources
    ${_moguet_package_relation_assessment_test_sources}
)
moguet_add_cpp_test(
    package-relation-assessment-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_package_relation_assessment_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES
        ${_moguet_package_relation_assessment_forbidden_sources}
)

set(
    _moguet_package_constraint_metadata_test_sources
    tests/package_constraint_metadata_test.cpp
    source/installed_package_relation_inventory.cpp
    source/package_constraint_metadata.cpp
    source/package_metadata.cpp
    source/package_relation_observation_adapter.cpp
    source/package_relation_observation.cpp
    source/package_relation.cpp
    source/dependency_constraint.cpp
    source/package_identifier.cpp
    source/shell_words.cpp
    tests/stubs/package-metadata/alpm_stub.cpp
    tests/stubs/package-metadata/process_stub.cpp
)
_moguet_test_production_complement(
    _moguet_package_constraint_metadata_forbidden_sources
    ${_moguet_package_constraint_metadata_test_sources}
)
moguet_add_cpp_test(
    package-constraint-metadata-test
    FIREWALL
    ALPM_COMPILE
    SOURCES ${_moguet_package_constraint_metadata_test_sources}
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_package_constraint_metadata_forbidden_sources}
)

set(
    _moguet_aur_constraint_metadata_test_sources
    tests/aur_constraint_metadata_test.cpp
    source/aur_constraint_metadata.cpp
    source/package_relation.cpp
    source/dependency_constraint.cpp
)
_moguet_test_production_complement(
    _moguet_aur_constraint_metadata_forbidden_sources
    ${_moguet_aur_constraint_metadata_test_sources}
)
moguet_add_cpp_test(
    aur-constraint-metadata-test
    FIREWALL
    ALPM_COMPILE
    REAL_ALPM
    SOURCES ${_moguet_aur_constraint_metadata_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_aur_constraint_metadata_forbidden_sources}
)

moguet_add_cpp_test(
    package-metadata-integration-test
    ALPM_COMPILE
    REAL_ALPM
    SOURCES
        tests/package_metadata_integration_test.cpp
        source/package_metadata.cpp
        source/package_identifier.cpp
        source/shell_words.cpp
        source/process.cpp
        source/logging.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

set(
    MOGUET_EXPECTED_CPP_TEST_TARGETS
    installed-package-record-observation-test
    exact-artifact-transaction-protocol-test
    moguet-aur-update-command-test
    moguet-upgrade-all-command-test
    moguet-commands-sync-test
    moguet-commands-inspect-test
    moguet-test
    moguet-cli-localization-test
    moguet-app-config-test
    moguet-aur-rpc-validation-test
    moguet-source-install-characterization-test
    moguet-upgrade-baseline-metadata-test
    application-identity-test
    interactive-confirmation-test
    localization-test
    localization-missing-catalog-test
    xdg-paths-test
    xdg-directory-safety-test
    xdg-generation-store-test
    xdg-state-log-test
    trusted-cache-test
    moguet-root-execution-identity-test
    aur-rpc-envelope-validation-test
    app-config-test
    provider-selection-test
    root-package-candidate-test
    root-package-search-test
    root-package-selection-test
    root-package-route-projection-test
    local-package-metadata-test
    local-source-root-test
    local-dependency-plan-projection-test
    local-source-workspace-test
    local-source-build-test
    user-config-test
    package-identifier-test
    source-package-identity-test
    installed-artifact-binding-test
    devel-build-provenance-test
    devel-build-provenance-store-test
    git-remote-revision-observer-test
    source-package-identity-projection-test
    source-package-compatibility-test
    invocation-owned-cleanup-model-test
    source-artifact-install-trusted-transport-test
    reviewed-source-state-test
    reviewed-source-state-store-test
    reviewed-source-lifecycle-test
    reviewed-source-acceptance-test
    reviewed-source-pinned-build-test
    invocation-owned-source-build-context-test
    evaluated-devel-source-artifact-transport-test
    evaluated-devel-source-build-test
    reviewed-source-production-connection-test
    reviewed-source-projection-test
    reviewed-source-review-test
    reviewed-source-patch-test
    reviewed-source-presentation-test
    reviewed-source-git-test
    shell-words-test
    source-environment-test
    artifact-workspace-test
    multiple-artifact-workspace-test
    makepkg-assignment-precedence-test
    artifact-identity-test
    multiple-artifact-identity-test
    package-base-artifact-install-plan-test
    artifact-install-executor-test
    package-base-artifact-install-executor-test
    separated-source-build-test
    separated-package-base-source-build-test
    production-source-build-test
    process-capture-test
    bounded-process-test
    makepkg-devel-phase-characterization-test
    process-stdin-fd-test
    aur-update-plan-test
    upgrade-all-plan-test
    system-source-upgrade-test
    aur-update-query-test
    aur-update-execution-preflight-test
    aur-update-execution-preflight-integration-test
    aur-update-execution-preparation-test
    aur-update-execution-preparation-integration-test
    aur-update-execution-runner-test
    aur-update-operation-result-test
    filtered-aur-update-operation-test
    upgrade-all-operation-test
    cli-diagnostic-model-test
    runtime-cli-connection-test
    dependency-plan-model-test
    build-plan-artifact-target-projection-test
    unified-plan-observation-test
    unified-plan-projection-test
    unified-plan-renderer-test
    repository-query-test
    artifact-install-plan-test
    artifact-selection-model-test
    artifact-identity-selection-test
    package-metadata-test
    provider-installed-state-test
    dependency-constraint-test
    cross-source-version-lock-test
    package-relation-test
    package-relation-observation-test
    package-relation-assessment-test
    package-constraint-metadata-test
    aur-constraint-metadata-test
    package-metadata-integration-test
)

set(
    MOGUET_EXPECTED_CPP_TEST_SUPPORT_SOURCES
    tests/commands_inspect_aur_stub.cpp
    tests/stubs/artifact-identity/archive_metadata_stub.cpp
    tests/stubs/artifact-identity/process_stub.cpp
    tests/stubs/artifact-install-executor/process_stub.cpp
    tests/stubs/aur-update-command/operation_stub.cpp
    tests/stubs/aur-update-execution-preflight-integration/integration_stub.cpp
    tests/stubs/aur-update-execution-preflight/preflight_stub.cpp
    tests/stubs/aur-update-execution-preparation-integration/preparation_stub.cpp
    tests/stubs/aur-update-execution-preparation/preparation_stub.cpp
    tests/stubs/aur-update-execution-runner/execution_stub.cpp
    tests/stubs/build-plan-relation-assessment/assessment_stub.cpp
    tests/stubs/commands-inspect/repository_query_stub.cpp
    tests/stubs/commands-sync/aur_rpc_stub.cpp
    tests/stubs/commands-sync/root_package_search_stub.cpp
    tests/stubs/dependency-plan/aur_rpc_stub.cpp
    tests/stubs/dependency-plan/repository_query_stub.cpp
    tests/stubs/filtered-aur-update-operation/query_stub.cpp
    tests/stubs/local-dependency-plan/aur_rpc_stub.cpp
    tests/stubs/local-dependency-plan/repository_query_stub.cpp
    tests/stubs/local-source-build/process_stub.cpp
    tests/stubs/package-metadata/alpm_stub.cpp
    tests/stubs/package-metadata/process_stub.cpp
    tests/stubs/repository-query/process_stub.cpp
    tests/stubs/reviewed-source-production/execution_stub.cpp
    tests/stubs/root-package-search/search_stub.cpp
    tests/stubs/runtime-identity/geteuid_stub.cpp
    tests/stubs/system-source-upgrade/phase_stub.cpp
    tests/stubs/trusted-git/process_stub.cpp
    tests/stubs/upgrade-all-command/operation_stub.cpp
    tests/stubs/upgrade-all-operation/operation_stub.cpp
)

set(
    MOGUET_EXPECTED_CPP_TEST_FIREWALL_TARGETS
    moguet-aur-update-command-test
    moguet-upgrade-all-command-test
    moguet-commands-sync-test
    moguet-commands-inspect-test
    moguet-test
    moguet-cli-localization-test
    moguet-app-config-test
    moguet-aur-rpc-validation-test
    moguet-source-install-characterization-test
    moguet-upgrade-baseline-metadata-test
    root-package-candidate-test
    root-package-search-test
    root-package-selection-test
    root-package-route-projection-test
    local-package-metadata-test
    local-source-root-test
    local-dependency-plan-projection-test
    local-source-workspace-test
    local-source-build-test
    source-package-identity-projection-test
    multiple-artifact-workspace-test
    makepkg-assignment-precedence-test
    multiple-artifact-identity-test
    package-base-artifact-install-plan-test
    package-base-artifact-install-executor-test
    separated-package-base-source-build-test
    upgrade-all-plan-test
    system-source-upgrade-test
    aur-update-execution-preflight-test
    aur-update-execution-runner-test
    aur-update-operation-result-test
    filtered-aur-update-operation-test
    upgrade-all-operation-test
    cli-diagnostic-model-test
    runtime-cli-connection-test
    dependency-plan-model-test
    build-plan-artifact-target-projection-test
    unified-plan-observation-test
    unified-plan-projection-test
    unified-plan-renderer-test
    artifact-selection-model-test
    artifact-identity-selection-test
    provider-installed-state-test
    dependency-constraint-test
    package-relation-test
    package-relation-observation-test
    package-relation-assessment-test
    package-constraint-metadata-test
    aur-constraint-metadata-test
    evaluated-devel-source-build-test
)

# These hashes are an independent fail-closed ledger for the 50 link
# firewalls.  Before changing any entry, compare the target's complete CMake
# source/link profile with the legacy Make closure and re-establish parity;
# never derive or update this expected ledger from the configure-time actual
# descriptors automatically.
set(
    MOGUET_EXPECTED_CPP_TEST_FIREWALL_DESCRIPTORS
    evaluated-devel-source-build-test=35ab850b88d57395cc8d58343439986708b04659a21575d2c59a397a53f01f03
    moguet-aur-update-command-test=f7f75e3f779b253b27d71e7976e3d75b77d719f741c1dd003bc68fe42953b149
    moguet-upgrade-all-command-test=6d3e41352286300eaa543afd35b30a2047d18c679dedd6efa0beabfa0cd0f2dc
    moguet-commands-sync-test=9fe98d2008b0b1e70b5cbd0f5a56cc878164affea76b40585b66e29e8ecb64bb
    moguet-commands-inspect-test=040e6efe099af897a02c01278b167e3d276c5902e5a5804bf90f953c86a6fd6e
    moguet-test=129478827c1b0134b6496bad1479bedbda6a3c9b3c197ce42b50eadc30334bac
    moguet-cli-localization-test=081c848985bddc6456e44a58d729cad9bbee86bb493cc64b69537caf84dca229
    moguet-app-config-test=61e2261d503bb6d46dd15a7945aebbb3e75ed2ec1280db1f3913e2f4a8e34966
    moguet-aur-rpc-validation-test=a707a6a2136b41acd198936cce5a724d811c263ddf5b1dbd644ff9387b3b774c
    moguet-source-install-characterization-test=43dbd71d23e7cdb3ac4235d4b1df8641dda6064f5e3e2274a04155e1260a8ac0
    moguet-upgrade-baseline-metadata-test=40958eb153cf89ba47ca6bdfa6b2ae41d717761a94ad26896e3ab8d6a69145f1
    root-package-candidate-test=0624a28a297f085803257435b9bcd024cdcebaaeb29d721177554d79e67fd9c1
    root-package-search-test=cf1cb86f3357240da0eec87c35de978909de056ce741923cef7bb9ff2cdcabe1
    root-package-selection-test=d8a9dbda1c56b94750848da47693ec32fb6d5c28fdd996671bbeeacd9d4c885f
    root-package-route-projection-test=ff26d1f205cdba67837c97ecddcc6225509917df4e90086286319e424394f22a
    local-package-metadata-test=7dca2816741d071af511180294e67edcd494986efd72c32e82691df445038d4a
    local-source-root-test=b867e14d4e5f66a05de5700f4d11e746095a0b397e2333a1c080c2a4e040f5a8
    local-dependency-plan-projection-test=22444c72d7fa2cdd401acf719d5b725c3f1b1357d1f84e728d5c4ed9becdb229
    local-source-workspace-test=70774b7f9c48e755e94a6167d5d59f92ca264dea221f7382711459c66dd6dd55
    local-source-build-test=9b5f3bf906cd90283777d76c68b5a04bcb542750ad8b31ab205726bfe13020c6
    source-package-identity-projection-test=2d0e55fc2cb5407d6ff4de1b10722c6878d5230865e47fc4865ac870c49b4bcd
    multiple-artifact-workspace-test=954337df69ede5e3e9a777c3542b91cf13a633cf1ca77361a1ec135549d656cf
    makepkg-assignment-precedence-test=05cd04c5728f45b4aad185eb9ab047163594b1a1e6cc5c56153f25af5d95487b
    multiple-artifact-identity-test=ac6ac72a7ad6c386a80e69b4717b53d389851f7688e779f24b860370df3aa466
    package-base-artifact-install-plan-test=b071634b64b32ac17c67c8c58132f9ae3588dae1aede7524790a09e0e80aaa93
    package-base-artifact-install-executor-test=5f4bbe4059fb2b296b3f6f25448a095f4fc5c5bda9c05a5811dd3bb66f9e71a6
    separated-package-base-source-build-test=2b63c74615539369a995f109b07b83813fc4731f3c30d656810df6191577aee1
    upgrade-all-plan-test=038ac56feb5677cca69820797efef71d51e6e5010916b65254d93dad16da8ec4
    system-source-upgrade-test=29460b16ebd1573b2e193c2c59df05eeb43f604a76f07ba521af51a0f9dc6937
    aur-update-execution-preflight-test=167eca6cef76a54712dd3281a015d38e406ae9fde1a264919d69b115a743cfd3
    aur-update-execution-runner-test=6bc5943cf6a1bdd3dacb4956d45aa9e670ae0467dc417176b9c4b9c0b2001818
    aur-update-operation-result-test=794c70c37241de19fa40d3e5369fadebe282fb321fd0e3771b8b3d89e0a369d2
    filtered-aur-update-operation-test=52a7fa05e9c75c4115f2ceaf9541592073ccc6be3bf3483197262c272f1f3fdb
    upgrade-all-operation-test=cf44453fe212765c7ff9fc72c6b59e04a3fe8274866df781f401b8ef3bf7a1bc
    cli-diagnostic-model-test=bcfe04e89780bc70c0475753961e9dac22dc424cca04587702829d14dd8d0ef4
    runtime-cli-connection-test=d24c5a74ef60d7f81bbd87258938d7370ebe4e5b6847c9df2cba1b0d8d522e51
    dependency-plan-model-test=8b4e882c854b10ff431de9a7a48c8989c33731efde9636df6d20ebb1d926e1a6
    build-plan-artifact-target-projection-test=13dc0efb1a18c8b2526df71ce789a83ce5f845e3f891520eea25974301687bdb
    unified-plan-observation-test=1aceeff1637fb52c41cd12c64e2518caae31f4c14d89e0672e5620dd23df39e7
    unified-plan-projection-test=ec546b629c0a904753b6021b77ab7373f4394ff5e7d6f1aba3d61fa4e9e968f2
    unified-plan-renderer-test=76bac81c96b535ef693b5d40fb67b10b308cb89369c208863babf969d9eda430
    artifact-selection-model-test=d372826d14f1cfb8aed85f85896d62813037b0c5c76214bf996b489ba4db10a3
    artifact-identity-selection-test=25373f2972b271c95ec29254a1b9be7629e8ec25926ee365adec80f6fa2d90d8
    provider-installed-state-test=1b664fc47baf2a86e6a1cd9c43a9315ba324dd3464abc5ca5983f4c9f7e88ef9
    dependency-constraint-test=9601dfe026af94d2b353f4f0938af55b3b291c09fa44434c1231fd4da10956f0
    package-relation-test=1dd9221d80a75e112e111d7eeab4dc7043a4760ad48e4e7208b4305e079d1193
    package-relation-observation-test=6ce1410d85354abaf6d3a5dad7dfb2f18b46b67ae4d09d0bd25b4cf0ef3e4605
    package-relation-assessment-test=25200fa6a53574fc0314a898e4e233be355a8f8f31531ad614e8dc79b2d5453d
    package-constraint-metadata-test=712605739d510301056359127f071c28eada0f6241bb82afd30d3f356216fdb8
    aur-constraint-metadata-test=b8f28ee8051628461faf6ca4fef869926738e4aa5184649588baff63a28e3e11
)
