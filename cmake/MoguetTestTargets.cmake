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
    source/aur_devel_update.cpp
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
    tests/stubs/aur-devel-update/query_stub.cpp
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
        MOGUET_ENABLE_CLI_INSTALL_TEST_ADAPTER
        "MOGUET_TEST_LEGACY_INSTALL_ADAPTER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy-install-test-adapter.py\""
        MOGUET_ENABLE_TEST_CONFIG_PATH
        MOGUET_ENABLE_SYSTEM_AUR_UPDATE_PRESENTATION_TEST_HOOKS
        "MOGUET_LOCALE_DIRECTORY=\"${CMAKE_CURRENT_BINARY_DIR}/locale\""
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_aur_update_command_forbidden_sources}
)
add_dependencies(moguet-aur-update-command-test moguet_catalogs)

set(
    _moguet_upgrade_all_command_test_sources
    ${MOGUET_PRODUCTION_SOURCES}
)
list(
    REMOVE_ITEM _moguet_upgrade_all_command_test_sources
    source/aur_devel_update.cpp
    source/artifact_archive_metadata.cpp
    source/upgrade_all_operation.cpp
)
list(
    APPEND _moguet_upgrade_all_command_test_sources
    tests/stubs/aur-devel-update/query_stub.cpp
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
        MOGUET_ENABLE_CLI_INSTALL_TEST_ADAPTER
        "MOGUET_TEST_LEGACY_INSTALL_ADAPTER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy-install-test-adapter.py\""
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
    source/aur_devel_update.cpp
    source/artifact_archive_metadata.cpp
    source/aur_rpc.cpp
    source/root_package_search.cpp
)
list(
    APPEND _moguet_commands_sync_test_sources
    tests/stubs/aur-devel-update/query_stub.cpp
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
        MOGUET_ENABLE_CLI_INSTALL_TEST_ADAPTER
        "MOGUET_TEST_LEGACY_INSTALL_ADAPTER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy-install-test-adapter.py\""
        MOGUET_ENABLE_TEST_CONFIG_PATH
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_commands_sync_forbidden_sources}
)

set(_moguet_commands_inspect_test_sources ${MOGUET_PRODUCTION_SOURCES})
list(
    REMOVE_ITEM _moguet_commands_inspect_test_sources
    source/aur_devel_update.cpp
    source/artifact_archive_metadata.cpp
    source/aur_rpc.cpp
    source/repository_query.cpp
)
list(
    APPEND _moguet_commands_inspect_test_sources
    tests/stubs/aur-devel-update/query_stub.cpp
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
        MOGUET_ENABLE_CLI_INSTALL_TEST_ADAPTER
        "MOGUET_TEST_LEGACY_INSTALL_ADAPTER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy-install-test-adapter.py\""
        "MOGUET_LOCALE_DIRECTORY=\"${CMAKE_CURRENT_BINARY_DIR}/locale\""
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_alpm_stub_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_commands_inspect_forbidden_sources}
)

set(_moguet_full_cli_test_sources ${MOGUET_PRODUCTION_SOURCES})
list(
    REMOVE_ITEM _moguet_full_cli_test_sources
    source/aur_devel_update.cpp
    source/artifact_archive_metadata.cpp
)
list(
    APPEND _moguet_full_cli_test_sources
    tests/stubs/aur-devel-update/query_stub.cpp
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
        MOGUET_ENABLE_CLI_INSTALL_TEST_ADAPTER
        "MOGUET_TEST_LEGACY_INSTALL_ADAPTER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy-install-test-adapter.py\""
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
        MOGUET_ENABLE_CLI_INSTALL_TEST_ADAPTER
        "MOGUET_TEST_LEGACY_INSTALL_ADAPTER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy-install-test-adapter.py\""
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
        MOGUET_ENABLE_CLI_INSTALL_TEST_ADAPTER
        "MOGUET_TEST_LEGACY_INSTALL_ADAPTER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy-install-test-adapter.py\""
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
    source/aur_devel_update.cpp
    source/artifact_archive_metadata.cpp
)
list(
    APPEND _moguet_aur_rpc_validation_test_sources
    tests/stubs/aur-devel-update/query_stub.cpp
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
        MOGUET_ENABLE_CLI_INSTALL_TEST_ADAPTER
        "MOGUET_TEST_LEGACY_INSTALL_ADAPTER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy-install-test-adapter.py\""
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
    source/aur_devel_update.cpp
    source/artifact_archive_metadata.cpp
    source/moguet.cpp
)
list(
    APPEND _moguet_source_install_characterization_test_sources
    tests/stubs/aur-devel-update/query_stub.cpp
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
        MOGUET_ENABLE_CLI_INSTALL_TEST_ADAPTER
        "MOGUET_TEST_LEGACY_INSTALL_ADAPTER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy-install-test-adapter.py\""
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
    source/aur_devel_update.cpp
    source/artifact_archive_metadata.cpp
)
list(
    APPEND _moguet_upgrade_baseline_metadata_test_sources
    tests/stubs/aur-devel-update/query_stub.cpp
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
        MOGUET_ENABLE_CLI_INSTALL_TEST_ADAPTER
        "MOGUET_TEST_LEGACY_INSTALL_ADAPTER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy-install-test-adapter.py\""
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
    ALPM_COMPILE REAL_ALPM
    SOURCES
        tests/app_config_test.cpp
        source/app_config.cpp
        source/provider_selection.cpp
        source/selection_expression.cpp
        source/dependency_constraint.cpp
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
        source/selection_expression.cpp
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

moguet_add_cpp_test(
    selection-expression-test
    SOURCES
        tests/selection_expression_test.cpp
        source/selection_expression.cpp
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
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
    source/root_package_selection.cpp
    source/selection_expression.cpp
    source/root_package_route_projection.cpp
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
    source/selection_expression.cpp
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
    source/selection_expression.cpp
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

set(_moguet_local_recipe_candidate_test_sources
    ${_moguet_local_source_build_test_sources}
)
list(REMOVE_ITEM _moguet_local_recipe_candidate_test_sources
    tests/local_source_build_test.cpp
    tests/stubs/local-source-build/process_stub.cpp
    tests/stubs/artifact-identity/archive_metadata_stub.cpp
)
list(APPEND _moguet_local_recipe_candidate_test_sources
    tests/local_recipe_candidate_test.cpp
    source/local_recipe_candidate.cpp
    source/local_source_metadata_evaluation.cpp
    source/source_package_identity.cpp
    source/process.cpp
    source/artifact_archive_metadata.cpp
)
_moguet_test_production_complement(
    _moguet_local_recipe_candidate_forbidden_sources
    ${_moguet_local_recipe_candidate_test_sources}
)
moguet_add_cpp_test(
    local-recipe-candidate-test
    ALPM_COMPILE REAL_ALPM
    SOURCES ${_moguet_local_recipe_candidate_test_sources}
    DEFINITIONS MOGUET_ENABLE_LOCAL_SOURCE_WORKSPACE_TEST_HOOKS
    INCLUDE_DIRECTORIES
        "${_moguet_test_source_include_dir}"
        "${_moguet_test_support_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_local_recipe_candidate_forbidden_sources}
)

set(_moguet_local_patch_association_test_sources ${_moguet_local_recipe_candidate_test_sources})
list(REMOVE_ITEM _moguet_local_patch_association_test_sources tests/local_recipe_candidate_test.cpp)
list(APPEND _moguet_local_patch_association_test_sources
    tests/local_patch_association_test.cpp
    source/local_patch_association.cpp
    source/xdg_generation_store_sha256.cpp
)
_moguet_test_production_complement(
    _moguet_local_patch_association_forbidden_sources
    ${_moguet_local_patch_association_test_sources}
)
moguet_add_cpp_test(
    local-patch-association-test
    ALPM_COMPILE REAL_ALPM
    SOURCES ${_moguet_local_patch_association_test_sources}
    DEFINITIONS MOGUET_ENABLE_PATCH_ASSOCIATION_TEST_HOOKS MOGUET_ENABLE_LOCAL_SOURCE_WORKSPACE_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}" "${_moguet_test_support_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_local_patch_association_forbidden_sources}
)

set(_moguet_patch_cli_sources ${MOGUET_PRODUCTION_SOURCES})
# The existing devel-observation fixture isolates the fixed system DB world;
# patch selection, current checkout/review, planning, build and archive handling
# remain production code with real Git/makepkg/libalpm.
list(REMOVE_ITEM _moguet_patch_cli_sources source/aur_devel_update.cpp)
list(APPEND _moguet_patch_cli_sources tests/stubs/aur-devel-update/query_stub.cpp)
moguet_add_cpp_test(
    moguet-local-patch-cli-test
    ALPM_COMPILE REAL_ALPM CURL
    SOURCES ${_moguet_patch_cli_sources}
    DEFINITIONS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_CLI_INSTALL_TEST_ADAPTER
        "MOGUET_TEST_LEGACY_INSTALL_ADAPTER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy-install-test-adapter.py\""
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES source/aur_devel_update.cpp
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
    devel-package-assessment-test
    ALPM_COMPILE REAL_ALPM CURL
    SOURCES
        tests/devel_package_assessment_test.cpp
        source/devel_package_assessment.cpp
        source/devel_package_classification.cpp
        source/devel_update_model.cpp
        source/current_installed_artifact_binding_observer.cpp
        source/installed_package_record_observation.cpp
        source/source_artifact_install_trusted_protocol.cpp
        source/trusted_alpm_receipt_protocol.cpp
        source/devel_git_revision_comparison.cpp
        source/devel_build_provenance_store.cpp
        source/devel_build_provenance_codec.cpp
        source/git_remote_revision_observer.cpp
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
        MOGUET_ENABLE_DEVEL_PACKAGE_ASSESSMENT_TEST_HOOKS
        MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
        MOGUET_ENABLE_XDG_GENERATION_STORE_TEST_HOOKS
        MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_TEST_HOOKS
        MOGUET_ENABLE_GIT_REMOTE_REVISION_OBSERVER_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    aur-devel-route-test
    ALPM_COMPILE REAL_ALPM CURL
    SOURCES
        source/package_metadata.cpp
        source/aur_devel_update.cpp
        source/srcinfo_source_metadata.cpp
        source/source_entry_parser.cpp
        source/local_package_metadata.cpp
        source/aur_update_query.cpp
        source/aur_update_plan.cpp
        source/shell_words.cpp
        tests/devel_package_assessment_test.cpp
        source/devel_package_assessment.cpp
        source/devel_package_classification.cpp
        source/devel_update_model.cpp
        source/current_installed_artifact_binding_observer.cpp
        source/installed_package_record_observation.cpp
        source/source_artifact_install_trusted_protocol.cpp
        source/trusted_alpm_receipt_protocol.cpp
        source/devel_git_revision_comparison.cpp
        source/devel_build_provenance_store.cpp
        source/devel_build_provenance_codec.cpp
        source/git_remote_revision_observer.cpp
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
        MOGUET_TEST_AUR_DEVEL_ROUTING
        MOGUET_ENABLE_DEVEL_TRACKING_BOOTSTRAP_TEST_HOOKS
        MOGUET_ENABLE_AUR_DEVEL_UPDATE_TEST_HOOKS
        MOGUET_ENABLE_DEVEL_PACKAGE_ASSESSMENT_TEST_HOOKS
        MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
        MOGUET_ENABLE_XDG_GENERATION_STORE_TEST_HOOKS
        MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_TEST_HOOKS
        MOGUET_ENABLE_GIT_REMOTE_REVISION_OBSERVER_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    current-installed-artifact-binding-test
    ALPM_COMPILE REAL_ALPM
    SOURCES
        tests/current_installed_artifact_binding_observer_test.cpp
        source/current_installed_artifact_binding_observer.cpp
        source/installed_artifact_binding.cpp
        source/installed_package_record_observation.cpp
        source/source_package_identity.cpp
        source/source_artifact_install_trusted_protocol.cpp
        source/trusted_alpm_receipt_protocol.cpp
        source/xdg_generation_store_sha256.cpp
        source/package_identifier.cpp
    DEFINITIONS
        MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
        MOGUET_ENABLE_CURRENT_INSTALLED_ARTIFACT_BINDING_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
)

moguet_add_cpp_test(
    devel-git-revision-comparison-test
    SOURCES
        tests/devel_git_revision_comparison_test.cpp
        source/devel_git_revision_comparison.cpp
        source/vcs_source_identity.cpp
        source/source_package_identity.cpp
        source/package_identifier.cpp
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
        tests/dependency_cleanup_execution_test.cpp
        tests/source_artifact_install_receipt_evidence_test.cpp
        tests/trusted_alpm_receipt_test.cpp
        source/invocation_owned_cleanup_adapter.cpp
        source/remote_aur_cleanup_candidate_collector.cpp
        source/dependency_cleanup_interaction.cpp
        source/dependency_cleanup_execution.cpp
        source/interactive_confirmation.cpp
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
    source/devel_source_artifact_install.cpp
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
# all target. The host compile gate builds it without execution; only the
# owner-specific container lane runs it.
set(
    _moguet_source_artifact_install_installed_fixture_sources
    tests/stubs/upgrade-patch/empty_registry_stub.cpp
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
    source/interactive_confirmation.cpp
    source/dependency_cleanup_interaction.cpp
    source/dependency_cleanup_execution.cpp
    source/package_relation.cpp
    source/package_relation_presentation.cpp
    source/source_artifact_install_trusted_protocol.cpp
    source/source_artifact_install_trusted_transport.cpp
    source/devel_source_artifact_install.cpp
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

# The acquisition fixture uses the real bootstrap observer and existing review/pin/S3.
set(_moguet_recipe_acquisition_test_sources
    ${_moguet_invocation_owned_source_build_context_test_sources}
    tests/invocation_owned_recipe_acquisition_test.cpp
    source/invocation_owned_recipe_acquisition.cpp
    source/aur_devel_update.cpp
    source/aur_update_query.cpp
    source/aur_update_plan.cpp
    source/package_metadata.cpp
    source/local_package_metadata.cpp
    source/srcinfo_source_metadata.cpp
    source/source_entry_parser.cpp
    source/shell_words.cpp
    source/devel_package_assessment.cpp
    source/devel_package_classification.cpp
    source/devel_update_model.cpp
    source/current_installed_artifact_binding_observer.cpp
    source/installed_package_record_observation.cpp
    source/source_artifact_install_trusted_protocol.cpp
    source/trusted_alpm_receipt_protocol.cpp
    source/devel_git_revision_comparison.cpp
    source/devel_build_provenance_store.cpp
    source/devel_build_provenance_codec.cpp
    source/git_remote_revision_observer.cpp
)
list(REMOVE_ITEM _moguet_recipe_acquisition_test_sources tests/invocation_owned_source_build_context_test.cpp)
_moguet_test_production_complement(_moguet_recipe_acquisition_forbidden_sources ${_moguet_recipe_acquisition_test_sources})
moguet_add_cpp_test(
    invocation-owned-recipe-acquisition-test
    ALPM_COMPILE REAL_ALPM CURL
    SOURCES ${_moguet_recipe_acquisition_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_RECIPE_ACQUISITION_TEST_HOOKS
        MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
        MOGUET_ENABLE_DEVEL_TRACKING_BOOTSTRAP_TEST_HOOKS
        MOGUET_ENABLE_AUR_DEVEL_UPDATE_TEST_HOOKS
        MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_recipe_acquisition_forbidden_sources}
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
    source/pinned_submodule_closure.cpp
    source/pinned_submodule_closure_review.cpp
    source/pinned_submodule_workspace.cpp
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

# Slice 4A consumes the real pre-prepare selection fixture in a separate lane.
set(_moguet_pinned_closure_test_sources ${_moguet_evaluated_devel_source_build_test_sources})
_moguet_test_production_complement(_moguet_pinned_closure_forbidden_sources ${_moguet_pinned_closure_test_sources})
moguet_add_cpp_test(
    pinned-submodule-closure-test
    FIREWALL
    ALPM_COMPILE REAL_ALPM CURL
    SOURCES ${_moguet_pinned_closure_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_REVIEWED_SOURCE_PRESENTATION_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_ACCEPTANCE_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_STATE_STORE_TEST_HOOKS
        MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
        MOGUET_ENABLE_EVALUATED_DEVEL_SOURCE_BUILD_TEST_HOOKS
        MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}" "${_moguet_test_support_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_pinned_closure_forbidden_sources}
)

# 4B0 uses real selection/closure fixtures, but runs only the review lane.
set(_moguet_pinned_closure_review_test_sources ${_moguet_pinned_closure_test_sources})
_moguet_test_production_complement(_moguet_pinned_closure_review_forbidden_sources ${_moguet_pinned_closure_review_test_sources})
moguet_add_cpp_test(
    pinned-submodule-closure-review-test
    FIREWALL
    ALPM_COMPILE REAL_ALPM CURL
    SOURCES ${_moguet_pinned_closure_review_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_REVIEWED_SOURCE_PRESENTATION_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_ACCEPTANCE_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_STATE_STORE_TEST_HOOKS
        MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
        MOGUET_ENABLE_EVALUATED_DEVEL_SOURCE_BUILD_TEST_HOOKS
        MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
        MOGUET_ENABLE_PINNED_CLOSURE_REVIEW_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}" "${_moguet_test_support_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_pinned_closure_review_forbidden_sources}
)

# 4B1 reuses acquisition/acceptance setup; only workspace cases execute.
set(_moguet_pinned_workspace_test_sources ${_moguet_pinned_closure_review_test_sources})
_moguet_test_production_complement(_moguet_pinned_workspace_forbidden_sources ${_moguet_pinned_workspace_test_sources})
moguet_add_cpp_test(
    pinned-submodule-workspace-test
    FIREWALL
    ALPM_COMPILE REAL_ALPM CURL
    SOURCES ${_moguet_pinned_workspace_test_sources}
    DEFINITIONS
        MOGUET_ENABLE_TEST_OVERRIDES
        MOGUET_ENABLE_REVIEWED_SOURCE_PRESENTATION_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_ACCEPTANCE_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_SOURCE_STATE_STORE_TEST_HOOKS
        MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
        MOGUET_ENABLE_EVALUATED_DEVEL_SOURCE_BUILD_TEST_HOOKS
        MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
        MOGUET_ENABLE_PINNED_CLOSURE_REVIEW_TEST_HOOKS
        MOGUET_ENABLE_PINNED_SUBMODULE_WORKSPACE_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}" "${_moguet_test_support_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_pinned_workspace_forbidden_sources}
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
        tests/devel_source_artifact_install_fixture.cpp
        tests/devel_build_provenance_publication_fixture.cpp
        source/devel_build_provenance_publication.cpp
        source/devel_build_provenance_store.cpp
        source/devel_build_provenance_codec.cpp
    DEFINITIONS
        MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
        MOGUET_ENABLE_XDG_GENERATION_STORE_TEST_HOOKS
        MOGUET_ENABLE_DEVEL_SOURCE_ARTIFACT_INSTALL_TEST_HOOKS
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
    reviewed-devel-source-build-execution-test
    ALPM_COMPILE REAL_ALPM CURL
    SOURCES tests/stubs/upgrade-patch/empty_registry_stub.cpp ${_moguet_evaluated_transport_test_sources}
        source/reviewed_devel_source_build_execution.cpp
        source/source_build.cpp
        source/invocation_owned_recipe_acquisition.cpp
        source/reviewed_devel_source_route.cpp
        tests/stubs/aur-devel-update/query_stub.cpp
        source/aur_update_plan.cpp
        source/devel_package_classification.cpp
        source/devel_update_model.cpp
        source/reviewed_source_production_failure.cpp
        source/reviewed_source_production_outcome.cpp
        source/cache_authority.cpp
        tests/devel_source_artifact_install_fixture.cpp
        tests/devel_build_provenance_publication_fixture.cpp
        source/devel_build_provenance_publication.cpp
        source/devel_build_provenance_store.cpp
        source/devel_build_provenance_codec.cpp
    DEFINITIONS
        MOGUET_TEST_REVIEWED_DEVEL_SOURCE_EXECUTION
        MOGUET_ENABLE_REVIEWED_DEVEL_SOURCE_BUILD_EXECUTION_TEST_HOOKS
        MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
        MOGUET_ENABLE_XDG_GENERATION_STORE_TEST_HOOKS
        MOGUET_ENABLE_DEVEL_SOURCE_ARTIFACT_INSTALL_TEST_HOOKS
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
    normal-reviewed-devel-execution-test
    ALPM_COMPILE REAL_ALPM CURL
    SOURCES tests/stubs/upgrade-patch/empty_registry_stub.cpp ${_moguet_evaluated_transport_test_sources}
        source/app_config.cpp
        source/diagnostic_projection.cpp
        source/runtime_diagnostic.cpp
        source/invocation_owned_cleanup_adapter.cpp
        tests/stubs/reviewed-source-production/execution_stub.cpp
        source/reviewed_devel_source_build_execution.cpp
        source/source_build.cpp
        source/invocation_owned_recipe_acquisition.cpp
        source/source_install.cpp
        source/reviewed_devel_source_route.cpp
        tests/stubs/aur-devel-update/query_stub.cpp
        source/aur_update_plan.cpp
        source/devel_package_classification.cpp
        source/devel_update_model.cpp
        source/reviewed_source_production_failure.cpp
        source/reviewed_source_production_outcome.cpp
        source/cache_authority.cpp
        tests/devel_source_artifact_install_fixture.cpp
        tests/devel_build_provenance_publication_fixture.cpp
        source/devel_build_provenance_publication.cpp
        source/devel_build_provenance_store.cpp
        source/devel_build_provenance_codec.cpp
    DEFINITIONS
        MOGUET_TEST_NORMAL_REVIEWED_DEVEL_EXECUTION
        MOGUET_ENABLE_SOURCE_INVOCATION_EXECUTION_TEST_HOOKS
        MOGUET_TEST_REVIEWED_DEVEL_SOURCE_EXECUTION
        MOGUET_ENABLE_REVIEWED_DEVEL_SOURCE_BUILD_EXECUTION_TEST_HOOKS
        MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
        MOGUET_ENABLE_XDG_GENERATION_STORE_TEST_HOOKS
        MOGUET_ENABLE_DEVEL_SOURCE_ARTIFACT_INSTALL_TEST_HOOKS
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

# #553 retains the actual CLI classifier, system/AUR coordinator, planner,
# confirmation/review and S4/S5/S6 owners. Only network and privileged fixture
# effects are redirected; no synthetic Complete/provenance is injected.
set(_moguet_devel_bootstrap_test_sources ${MOGUET_PRODUCTION_SOURCES})
list(REMOVE_ITEM _moguet_devel_bootstrap_test_sources source/moguet.cpp)
moguet_add_cpp_test(
    devel-tracking-bootstrap-test
    ALPM_COMPILE REAL_ALPM CURL
    SOURCES ${_moguet_devel_bootstrap_test_sources}
        source/source_artifact_install_trusted_helper_state.cpp
        tests/evaluated_devel_source_build_test.cpp
        tests/devel_source_artifact_install_fixture.cpp
        tests/devel_build_provenance_publication_fixture.cpp
    DEFINITIONS
        MOGUET_TEST_DEVEL_BOOTSTRAP_INTEGRATION
        MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
        MOGUET_ENABLE_PINNED_CLOSURE_REVIEW_TEST_HOOKS
        MOGUET_ENABLE_PINNED_SUBMODULE_WORKSPACE_TEST_HOOKS
        MOGUET_ENABLE_RECIPE_ACQUISITION_TEST_HOOKS
        MOGUET_ENABLE_AUR_UPDATE_EXECUTION_RUNNER_TEST_HOOKS
        MOGUET_TEST_NORMAL_REVIEWED_DEVEL_EXECUTION
        MOGUET_TEST_REVIEWED_DEVEL_SOURCE_EXECUTION
        MOGUET_ENABLE_SOURCE_INVOCATION_EXECUTION_TEST_HOOKS
        MOGUET_ENABLE_REVIEWED_DEVEL_SOURCE_BUILD_EXECUTION_TEST_HOOKS
        MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
        MOGUET_ENABLE_XDG_GENERATION_STORE_TEST_HOOKS
        MOGUET_ENABLE_DEVEL_SOURCE_ARTIFACT_INSTALL_TEST_HOOKS
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
        MOGUET_ENABLE_AUR_DEVEL_UPDATE_TEST_HOOKS
        MOGUET_ENABLE_DEVEL_TRACKING_BOOTSTRAP_TEST_HOOKS
        MOGUET_ENABLE_DEVEL_PACKAGE_ASSESSMENT_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}" "${_moguet_test_support_include_dir}"
    COMPILE_OPTIONS -ffunction-sections -fdata-sections
    LINK_OPTIONS LINKER:--gc-sections
)

moguet_add_cpp_test(
    reviewed-source-production-connection-test
    SOURCES tests/stubs/upgrade-patch/empty_registry_stub.cpp
        tests/reviewed_source_production_connection_test.cpp
        source/source_build.cpp
        tests/stubs/aur-devel-update/execution_stub.cpp
        tests/stubs/aur-devel-update/query_stub.cpp
        source/aur_update_plan.cpp
        source/devel_package_classification.cpp
        source/devel_update_model.cpp
        source/vcs_source_identity.cpp
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
    source/source_artifact_install_trusted_protocol.cpp
    source/trusted_alpm_receipt_protocol.cpp
    source/xdg_generation_store_sha256.cpp
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
    source/source_artifact_install_trusted_protocol.cpp
    source/trusted_alpm_receipt_protocol.cpp
    source/xdg_generation_store_sha256.cpp
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
    SOURCES tests/stubs/upgrade-patch/empty_registry_stub.cpp
        tests/production_source_build_test.cpp
        source/app_config.cpp
        source/aur_constraint_metadata.cpp
        source/package_relation.cpp
        source/package_relation_observation.cpp
        source/package_relation_observation_adapter.cpp
        source/provider_selection.cpp
        source/selection_expression.cpp
        source/source_install.cpp
        source/local_source_build_dependency_preparation.cpp
        source/local_dependency_plan_projection.cpp
        source/dependency_constraint.cpp
        source/dependency_constraint_presentation.cpp
        source/local_package_metadata.cpp
        source/cache_authority.cpp
        source/source_install_preparation.cpp
        source/source_build.cpp
        tests/stubs/aur-devel-update/execution_stub.cpp
        tests/stubs/aur-devel-update/query_stub.cpp
        source/aur_update_plan.cpp
        source/devel_package_classification.cpp
        source/devel_update_model.cpp
        source/vcs_source_identity.cpp
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
        source/dependency_cleanup_interaction.cpp
        source/dependency_cleanup_execution.cpp
        source/diagnostic_projection.cpp
        source/runtime_diagnostic.cpp
        source/separated_source_build.cpp
        source/separated_package_base_source_build.cpp
        source/package_base_artifact_install_executor.cpp
        source/source_artifact_install_trusted_protocol.cpp
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
    tests/stubs/aur-devel-update/execution_stub.cpp
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
    SOURCES tests/stubs/upgrade-patch/empty_registry_stub.cpp ${_moguet_system_source_upgrade_test_sources}
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
        tests/stubs/aur-devel-update/query_stub.cpp
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
    SOURCES tests/stubs/upgrade-patch/empty_registry_stub.cpp
        source/devel_update_model.cpp
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
    SOURCES tests/stubs/upgrade-patch/empty_registry_stub.cpp
        source/devel_update_model.cpp
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
    source/devel_update_model.cpp
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
    SOURCES tests/stubs/upgrade-patch/empty_registry_stub.cpp ${_moguet_aur_update_execution_runner_test_sources}
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
    SOURCES tests/stubs/upgrade-patch/empty_registry_stub.cpp ${_moguet_aur_update_operation_result_test_sources}
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES ${_moguet_aur_update_operation_result_forbidden_sources}
)

set(
    _moguet_filtered_aur_update_operation_test_sources
    tests/filtered_aur_update_operation_test.cpp
    source/app_config.cpp
    source/provider_selection.cpp
    source/selection_expression.cpp
    source/filtered_aur_update_operation.cpp
    source/system_aur_update_operation.cpp
    source/interactive_confirmation.cpp
    source/cross_source_version_lock.cpp
    source/cross_source_version_lock_observation.cpp
    source/package_relation_observation.cpp
    source/upgrade_all_plan.cpp
    source/aur_update_query.cpp
    tests/stubs/aur-devel-update/query_stub.cpp
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
    SOURCES tests/stubs/upgrade-patch/empty_registry_stub.cpp ${_moguet_filtered_aur_update_operation_test_sources}
    DEFINITIONS
        MOGUET_TEST_REAL_INTERACTIVE_CONFIRMATION
        MOGUET_ENABLE_AUR_UPDATE_EXECUTION_PREPARATION_TEST_HOOKS
        MOGUET_ENABLE_AUR_UPDATE_EXECUTION_RUNNER_TEST_HOOKS
    INCLUDE_DIRECTORIES "${_moguet_test_source_include_dir}"
    FORBIDDEN_SOURCES
        ${_moguet_filtered_aur_update_operation_forbidden_sources}
)

set(
    _moguet_upgrade_all_operation_test_sources
    tests/stubs/aur-devel-update/execution_stub.cpp
    tests/upgrade_all_operation_test.cpp
    source/app_config.cpp
    source/provider_selection.cpp
    source/selection_expression.cpp
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
    tests/stubs/aur-devel-update/query_stub.cpp
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
    SOURCES tests/stubs/upgrade-patch/empty_registry_stub.cpp ${_moguet_upgrade_all_operation_test_sources}
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
    source/devel_update_model.cpp
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
    source/devel_update_model.cpp
    tests/unified_plan_projection_test.cpp
    source/unified_plan_observation.cpp
    source/build_plan_artifact_target_projection.cpp
    source/root_package_route_projection.cpp
    source/root_package_selection.cpp
    source/selection_expression.cpp
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
    source/source_package_identity.cpp
    source/vcs_source_identity.cpp
    source/aur_update_plan.cpp
    source/devel_package_classification.cpp
    source/devel_update_model.cpp
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
    devel-tracking-bootstrap-test
    normal-reviewed-devel-execution-test
    aur-devel-route-test
    reviewed-devel-source-build-execution-test
    devel-package-assessment-test
    current-installed-artifact-binding-test
    devel-git-revision-comparison-test
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
    selection-expression-test
    root-package-candidate-test
    root-package-search-test
    root-package-selection-test
    root-package-route-projection-test
    local-package-metadata-test
    local-source-root-test
    local-dependency-plan-projection-test
    local-source-workspace-test
    local-source-build-test
    local-recipe-candidate-test
    local-patch-association-test
    moguet-local-patch-cli-test
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
    invocation-owned-recipe-acquisition-test
    evaluated-devel-source-artifact-transport-test
    pinned-submodule-closure-test
    pinned-submodule-closure-review-test
    pinned-submodule-workspace-test
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
    tests/stubs/upgrade-patch/empty_registry_stub.cpp
    tests/stubs/aur-devel-update/execution_stub.cpp
    tests/stubs/aur-devel-update/query_stub.cpp
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
    pinned-submodule-closure-review-test
    pinned-submodule-workspace-test
    pinned-submodule-closure-test
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

# These hashes are an independent fail-closed ledger for the link
# firewalls.  Before changing any entry, compare the target's complete CMake
# source/link profile with the legacy Make closure and re-establish parity;
# never derive or update this expected ledger from the configure-time actual
# descriptors automatically.
set(
    MOGUET_EXPECTED_CPP_TEST_FIREWALL_DESCRIPTORS
    pinned-submodule-workspace-test=0ba611d6180a3f0e3b73c99817e89a2db6020598fd6be53ebd90a2c81246dc15
    pinned-submodule-closure-review-test=b2ab0db295e432ca43ad7c78dbcae1842c565546d26fb7fd31ef3d4a0a7ecd98
    pinned-submodule-closure-test=03f240a0ed88a9eeac4bbf5dec9a6f13a93d6aeed2b78a5198e8a7e710838856
    evaluated-devel-source-build-test=9fb987b70213c45ff860c99c7ed88517aae4347d259079ccda1b83b73335ce01
    moguet-aur-update-command-test=fd04215c457151d3fe28dde3535bf2dba2f8ca02df250a0f9b296b0f7d6331ab
    moguet-upgrade-all-command-test=e393b703edc543b8799a0a697f23d6ae36d233773ea8d923bea4ce56cfc5c690
    moguet-commands-sync-test=c734bb144ffae68f5fc12ad0f895f81ea102f8e81285289300b7c45c3de21505
    moguet-commands-inspect-test=5672e3eb50ec110dc66c7368b00294d68f3ebb838f9c487eba90e3d460489b6b
    moguet-test=d70729a4c67ddbfba5a8940c83c76992bef53fc9560ab52a556dea63d1840450
    moguet-cli-localization-test=2c5251afc89a985ee497b9c994007f688db5b9c465b3542095c05411eb4afc60
    moguet-app-config-test=55e8b84b2a5f3e0b0fdaa16bdc117c0e06c5918ac64f99472af5b68a538e3f28
    moguet-aur-rpc-validation-test=a48442b36eb138f7f2894cf6e8ca92e069d7a6738d9dcad069d63a0a224d3056
    moguet-source-install-characterization-test=83636d9a521bb794accd68213795067469ab251c4a79cb38207c2ec1d276e485
    moguet-upgrade-baseline-metadata-test=8cbac611633bba8149d14964b7711e2f5c342791771ef1fb1a2f8acc58fee9ec
    root-package-candidate-test=9a1ae8172ff037a9fc71c275f0a650113dc91202196d44cdd0e9ed603d5945d1
    root-package-search-test=d3057e21ab7d61b4cdb9b832afdfe4aeddad103400aeea7a96c843bcff2e08ac
    root-package-selection-test=dbc842ce211eb942db62ce0502e70a73e807bb307037bb909ca555ee970b69be
    root-package-route-projection-test=2355936debba5108dbd134855231edf7388d5ac6db231b026495dd214fa228bf
    local-package-metadata-test=1c9c01ce168960c1e64084beeba44c56662fd9db39f3acc482a44efe20e56961
    local-source-root-test=68d2420baf91dc65817422227842788bb384879fa135e02e2ae1af5ba959cc76
    local-dependency-plan-projection-test=baa89162fb563f733849c02b9fcb75d1e3eb17152347dcc6ae0ea19f3c9871fe
    local-source-workspace-test=8cb3e162bce8eb04f0b526ee9cd6392cc25c0568cfdf65ccbe209d22c24558e2
    local-source-build-test=ee56a446f48217e8b036138a36a10b6e97488387ca360aed118edc35ba062f65
    source-package-identity-projection-test=f6ff1f3b13b8ee84499c603d96a3b1960b5842f02d33ca19c596767280730f51
    multiple-artifact-workspace-test=5ef24d592d25d4286bc5b92d32212f86759bd306e4fff91380e2fe3048c59ade
    makepkg-assignment-precedence-test=be6e7cd95e5a555e914ea3e9b27b055c4535190fce77f384a7af8a9ab136ebb2
    multiple-artifact-identity-test=067988666e00b1cca271c0b786e066715a280076f2dcd45448c5657207a7baa9
    package-base-artifact-install-plan-test=368edc1d070b683d203847a63c648afb64293c52d199de5ececc63f97b20273a
    package-base-artifact-install-executor-test=a01047cc149d2f574c0fc7027078e4df59144ca178184ff3f7bbd1b521958516
    separated-package-base-source-build-test=7872f9746d40641626383fa83ad522084e6ba7074291d7384e54253b7f5309e8
    upgrade-all-plan-test=c3cd402c993f0b8b881f84de1ebab302d5a1c192952a2805b5325d603f82ece0
    system-source-upgrade-test=55048c76087cb4972e8adba1a8609bfd0613e5e8e3d96e7643815f2a40ce5074
    aur-update-execution-preflight-test=167eca6cef76a54712dd3281a015d38e406ae9fde1a264919d69b115a743cfd3
    aur-update-execution-runner-test=6d1b6fe458cc8deda8d86e0c7e2be424efe727653e062bf18b69f4a6dbae7ed8
    aur-update-operation-result-test=407614edbc98c79f1e66774eb04893c2c33cadfa1842e028c91397a07861dd25
    filtered-aur-update-operation-test=4bb6e186ef2dbe6bc4c5cd26c75881cbb5016910b18e439062f9d4d827a20f1e
    upgrade-all-operation-test=73a260225124c594eaf321e5a2a19951fb9834cd109c0bb20fe78bd5063baf66
    cli-diagnostic-model-test=72cf826bf70c767806812a6592379eb01a4353a44f3e64f5aa3a965ad23c7406
    runtime-cli-connection-test=9296d5564f7146fd95b0d823e154491d6cd27bb7041b5a689389df6a6d40dd31
    dependency-plan-model-test=6a378647ba5941d86f15ee491f8c1534bea2a43109dde99a5d939d802fbf3c3b
    build-plan-artifact-target-projection-test=5b7dff2d1c5f864e80015ec594a1c3c30e65fd4d5c04354d67cd5cb39f2b8cfb
    unified-plan-observation-test=08c635811ec93469fe35886c90dff30ee7805f3c03cd5b6d438b3774271c8fb1
    unified-plan-projection-test=8dbdfecefb4ab1b02a77a6f56556bd381ee1e9e09308ab0b3d000ddffa385192
    unified-plan-renderer-test=bd9936a8e91f1e03639db98a743d4b8b5d10269ea79c09f021f6189dc89728f8
    artifact-selection-model-test=a2dfc5492078fd158faf091a7d47035256749fab1e0ed8f11b93c5518ef956ed
    artifact-identity-selection-test=d8b01fe9eba6556bb97cfccfdc22b954ac6883bf694d4d27ddd38907c62abc1c
    provider-installed-state-test=a5244902a93dd5e57a059c81866b0121910597adfeab0f8fe4f28f4a8044a77e
    dependency-constraint-test=9d7bc0c74258ccb53925b2d4d8ea864e00a96ca2f327eb9a684fe53b1a49936d
    package-relation-test=966631e246621443110df3e4e0caf7230ffc70189b8e597cd67afa0ca5674f17
    package-relation-observation-test=14ece7c6a36b59ca1c27c8827189497949d507309202d10b690d46fa47785994
    package-relation-assessment-test=db73eb7ae886163f603b4d3da27f76e7390b28fb0665f9cd7dea36a64e66d65c
    package-constraint-metadata-test=ec927767aced708d8daa0c999bf9253f801cc2e1e1950e7c205b739d8fb9e06c
    aur-constraint-metadata-test=aaf0c2a0d81a61e62d7c273edec89a306c003eab45c573cbefe1b55b078510b4
)
