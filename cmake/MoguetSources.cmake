# Canonical production translation-unit manifest. Additions and removals are
# build-graph changes and must be explicit; do not replace this with a glob.
set(MOGUET_PRODUCTION_SOURCES
    source/app_config.cpp
    source/artifact_archive_metadata.cpp
    source/artifact_identity.cpp
    source/artifact_identity_selection.cpp
    source/artifact_identity_set.cpp
    source/artifact_install_executor.cpp
    source/artifact_install_plan.cpp
    source/artifact_workspace.cpp
    source/aur_constraint_metadata.cpp
    source/aur_rpc.cpp
    source/aur_update_cli_presentation.cpp
    source/aur_update_execution_preflight.cpp
    source/aur_update_execution_preparation.cpp
    source/aur_update_execution_runner.cpp
    source/aur_update_operation_result.cpp
    source/aur_update_plan.cpp
    source/aur_update_query.cpp
    source/aur_devel_update.cpp
    source/reviewed_devel_source_route.cpp
    source/build_plan_artifact_target_projection.cpp
    source/build_plan_relation_assessment.cpp
    source/cache_authority.cpp
    source/checkout_fetch.cpp
    source/cli_parser.cpp
    source/cli_public_projection.cpp
    source/cli_routing.cpp
    source/cli_runtime_contract.cpp
    source/commands_aur_update.cpp
    source/commands_inspect.cpp
    source/commands_source_maintenance.cpp
    source/commands_sync.cpp
    source/commands_upgrade_all.cpp
    source/cross_source_version_lock.cpp
    source/cross_source_version_lock_observation.cpp
    source/devel_build_provenance.cpp
    source/devel_build_provenance_codec.cpp
    source/devel_build_provenance_reviewed_binding.cpp
    source/devel_build_provenance_store.cpp
    source/devel_build_provenance_publication.cpp
    source/devel_source_artifact_install.cpp
    source/devel_package_classification.cpp
    source/devel_update_model.cpp
    source/current_installed_artifact_binding_observer.cpp
    source/devel_git_revision_comparison.cpp
    source/devel_package_assessment.cpp
    source/reviewed_devel_source_build_execution.cpp
    source/dependency_constraint.cpp
    source/dependency_constraint_presentation.cpp
    source/dependency_plan.cpp
    source/dependency_plan_model.cpp
    source/dependency_spec.cpp
    source/diagnostic_projection.cpp
    source/dry_run.cpp
    source/evaluated_devel_source_build.cpp
    source/exact_artifact_transaction_protocol.cpp
    source/filtered_aur_update_operation.cpp
    source/git_remote_revision_observer.cpp
    source/installed_artifact_binding.cpp
    source/installed_artifact_binding_observer.cpp
    source/installed_package_record_observation.cpp
    source/installed_package_relation_inventory.cpp
    source/interactive_confirmation.cpp
    source/invocation_owned_cleanup_adapter.cpp
    source/invocation_owned_cleanup_model.cpp
    source/invocation_owned_source_build_context.cpp
    source/local_dependency_plan_projection.cpp
    source/local_package_metadata.cpp
    source/local_source_build.cpp
    source/local_source_build_dependency_preparation.cpp
    source/local_source_install.cpp
    source/local_source_metadata_evaluation.cpp
    source/local_source_root.cpp
    source/local_source_workspace.cpp
    source/localization.cpp
    source/logging.cpp
    source/moguet.cpp
    source/operation_state_model.cpp
    source/package_base_artifact_install_executor.cpp
    source/package_base_artifact_install_plan.cpp
    source/package_constraint_metadata.cpp
    source/package_identifier.cpp
    source/package_metadata.cpp
    source/package_relation.cpp
    source/package_relation_assessment.cpp
    source/package_relation_observation.cpp
    source/package_relation_observation_adapter.cpp
    source/package_relation_presentation.cpp
    source/persistent_checkout.cpp
    source/pkgbuild_export.cpp
    source/presentation_projection.cpp
    source/process.cpp
    source/provider_installed_state.cpp
    source/provider_installed_state_presentation.cpp
    source/provider_selection.cpp
    source/remote_aur_cleanup_candidate_collector.cpp
    source/repository_query.cpp
    source/reviewed_source_acceptance.cpp
    source/reviewed_source_git_parser.cpp
    source/reviewed_source_lifecycle.cpp
    source/reviewed_source_package_base_lease.cpp
    source/reviewed_source_patch.cpp
    source/reviewed_source_pinned_build.cpp
    source/reviewed_source_presentation.cpp
    source/reviewed_source_production_failure.cpp
    source/reviewed_source_production_outcome.cpp
    source/reviewed_source_projection.cpp
    source/reviewed_source_review.cpp
    source/reviewed_source_state.cpp
    source/reviewed_source_state_store.cpp
    source/reviewed_source_trusted_review.cpp
    source/root_package_candidate.cpp
    source/root_package_route_projection.cpp
    source/root_package_search.cpp
    source/root_package_selection.cpp
    source/runtime_diagnostic.cpp
    source/separated_package_base_source_build.cpp
    source/separated_source_build.cpp
    source/shell_words.cpp
    source/source_artifact_install_receipt_evidence.cpp
    source/source_artifact_install_trusted_protocol.cpp
    source/source_artifact_install_trusted_transport.cpp
    source/source_build.cpp
    source/source_entry_parser.cpp
    source/source_environment.cpp
    source/source_install.cpp
    source/source_install_preparation.cpp
    source/source_package_compatibility.cpp
    source/source_package_identity.cpp
    source/source_package_identity_projection.cpp
    source/source_preference.cpp
    source/srcinfo_source_metadata.cpp
    source/system_aur_update_operation.cpp
    source/system_source_upgrade.cpp
    source/trusted_cache.cpp
    source/trusted_git.cpp
    source/trusted_git_process_policy.cpp
    source/trusted_alpm_receipt_protocol.cpp
    source/trusted_alpm_receipt_transport.cpp
    source/unified_plan_observation.cpp
    source/unified_plan_projection.cpp
    source/unified_plan_renderer.cpp
    source/upgrade_all_operation.cpp
    source/upgrade_all_operation_result.cpp
    source/upgrade_all_plan.cpp
    source/user_config.cpp
    source/vcs_source_identity.cpp
    source/xdg_directory_safety.cpp
    source/xdg_generation_store.cpp
    source/xdg_generation_store_sha256.cpp
    source/xdg_paths.cpp
    source/xdg_state_log.cpp
)

# The package-installed root helper is a separate small executable and does
# not link the Moguet production object graph. Keep its exact source closure
# explicit alongside the main production manifest.
set(MOGUET_ALPM_RECEIPT_HELPER_SOURCES
    source/trusted_alpm_receipt_helper_main.cpp
    source/trusted_alpm_receipt_helper_state.cpp
    source/trusted_alpm_receipt_protocol.cpp
)

# SourceArtifactInstall owns a distinct executable, protocol, and state
# namespace. Only the token/name validators are shared with the existing
# selected-provider receipt protocol.
set(MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_SOURCES
    source/source_artifact_install_trusted_helper_main.cpp
    source/source_artifact_install_trusted_helper_state.cpp
    source/exact_artifact_transaction_protocol.cpp
    source/installed_package_record_observation.cpp
    source/xdg_generation_store_sha256.cpp
    source/source_artifact_install_trusted_protocol.cpp
    source/trusted_alpm_receipt_protocol.cpp
    source/package_identifier.cpp
)

set(_moguet_unique_alpm_receipt_helper_sources
    ${MOGUET_ALPM_RECEIPT_HELPER_SOURCES}
)
list(REMOVE_DUPLICATES _moguet_unique_alpm_receipt_helper_sources)
list(LENGTH MOGUET_ALPM_RECEIPT_HELPER_SOURCES _moguet_helper_source_count)
list(
    LENGTH
    _moguet_unique_alpm_receipt_helper_sources
    _moguet_unique_helper_source_count
)
if(NOT _moguet_helper_source_count EQUAL _moguet_unique_helper_source_count)
    message(
        FATAL_ERROR
        "MOGUET_ALPM_RECEIPT_HELPER_SOURCES contains duplicate entries"
    )
endif()
foreach(_moguet_helper_source IN LISTS MOGUET_ALPM_RECEIPT_HELPER_SOURCES)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_moguet_helper_source}")
        message(
            FATAL_ERROR
            "ALPM receipt helper source does not exist: "
            "${_moguet_helper_source}"
        )
    endif()
endforeach()
unset(_moguet_helper_source)
unset(_moguet_helper_source_count)
unset(_moguet_unique_alpm_receipt_helper_sources)
unset(_moguet_unique_helper_source_count)

set(_moguet_unique_source_artifact_helper_sources
    ${MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_SOURCES}
)
list(REMOVE_DUPLICATES _moguet_unique_source_artifact_helper_sources)
list(
    LENGTH
    MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_SOURCES
    _moguet_source_artifact_helper_source_count
)
list(
    LENGTH
    _moguet_unique_source_artifact_helper_sources
    _moguet_unique_source_artifact_helper_source_count
)
if(
    NOT _moguet_source_artifact_helper_source_count
        EQUAL _moguet_unique_source_artifact_helper_source_count
)
    message(
        FATAL_ERROR
        "MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_SOURCES contains duplicate entries"
    )
endif()
foreach(
    _moguet_source_artifact_helper_source
    IN LISTS MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_SOURCES
)
    if(
        NOT EXISTS
            "${CMAKE_CURRENT_SOURCE_DIR}/${_moguet_source_artifact_helper_source}"
    )
        message(
            FATAL_ERROR
            "Source-artifact install helper source does not exist: "
            "${_moguet_source_artifact_helper_source}"
        )
    endif()
endforeach()
unset(_moguet_source_artifact_helper_source)
unset(_moguet_source_artifact_helper_source_count)
unset(_moguet_unique_source_artifact_helper_source_count)
unset(_moguet_unique_source_artifact_helper_sources)

set(_moguet_unique_production_sources ${MOGUET_PRODUCTION_SOURCES})
list(REMOVE_DUPLICATES _moguet_unique_production_sources)
list(LENGTH MOGUET_PRODUCTION_SOURCES _moguet_production_source_count)
list(LENGTH _moguet_unique_production_sources _moguet_unique_source_count)
if(NOT _moguet_production_source_count EQUAL _moguet_unique_source_count)
    message(FATAL_ERROR "MOGUET_PRODUCTION_SOURCES contains duplicate entries")
endif()

foreach(_moguet_source IN LISTS MOGUET_PRODUCTION_SOURCES)
    if(NOT _moguet_source MATCHES "^source/[^/]+\\.cpp$")
        message(
            FATAL_ERROR
            "Production source is outside the source/*.cpp boundary: "
            "${_moguet_source}"
        )
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_moguet_source}")
        message(FATAL_ERROR "Production source does not exist: ${_moguet_source}")
    endif()
endforeach()

unset(_moguet_production_source_count)
unset(_moguet_source)
unset(_moguet_unique_production_sources)
unset(_moguet_unique_source_count)
