# Supported `make test-<area>` frontends.  CMake owns the exact build targets
# and CTest selections; Make only requests the corresponding custom target.
moguet_add_focused_ctest_alias(
    test-exact-artifact-transaction-receipt
    TESTS cpp.exact_artifact_transaction_receipt
    TARGETS source-artifact-install-trusted-transport-test
)

foreach(_moguet_direct_focus IN ITEMS
    application-identity
    interactive-confirmation
    xdg-paths
    xdg-directory-safety
    xdg-generation-store
    xdg-state-log
    trusted-cache
    provider-selection
    provider-installed-state
    dependency-constraint
    cross-source-version-lock
    package-relation
    package-relation-observation
    package-relation-assessment
    package-constraint-metadata
    aur-constraint-metadata
    root-package-candidate
    root-package-search
    root-package-selection
    root-package-route-projection
    local-package-metadata
    local-source-root
    local-dependency-plan-projection
    local-source-workspace
    local-source-build
    package-identifier
    source-package-identity
    exact-artifact-transaction-protocol
    installed-artifact-binding
    installed-package-record-observation
    devel-build-provenance
    devel-build-provenance-store
    source-package-identity-projection
    source-package-compatibility
    invocation-owned-cleanup-model
    invocation-owned-source-build-context
    evaluated-devel-source-build
    evaluated-devel-source-artifact-transport
    exact-installed-binding
    installed-devel-source-build-proof
    devel-source-artifact-install-result
    source-artifact-install-trusted-transport
    reviewed-source-state
    reviewed-source-state-store
    reviewed-source-lifecycle
    reviewed-source-acceptance
    reviewed-source-production-connection
    reviewed-source-projection
    reviewed-source-review
    reviewed-source-patch
    reviewed-source-presentation
    reviewed-source-git
    package-metadata
    package-metadata-integration
    shell-words
    source-environment
    artifact-workspace
    multiple-artifact-workspace
    makepkg-assignment-precedence
    makepkg-devel-phase-characterization
    artifact-identity
    multiple-artifact-identity
    package-base-artifact-install-plan
    artifact-install-executor
    package-base-artifact-install-executor
    separated-source-build
    separated-package-base-source-build
    production-source-build
    process-capture
    bounded-process
    aur-update-plan
    upgrade-all-plan
    system-source-upgrade
    aur-update-query
    aur-update-execution-preflight
    aur-update-execution-runner
    aur-update-operation-result
    filtered-aur-update-operation
    upgrade-all-operation
    cli-diagnostic-model
    runtime-cli-connection
    dependency-plan-model
    build-plan-artifact-target-projection
    unified-plan-observation
    artifact-install-plan
    artifact-selection-model
    artifact-identity-selection
)
    string(REPLACE "-" "_" _moguet_direct_ctest "${_moguet_direct_focus}")
    moguet_add_focused_ctest_alias(
        "test-${_moguet_direct_focus}"
        TESTS "cpp.${_moguet_direct_ctest}"
    )
endforeach()

moguet_add_focused_ctest_alias(
    test-git-remote-revision-observer
    TESTS
        cpp.git_remote_revision_observer
        cpp.git_remote_revision_observer_integration
)
moguet_add_focused_ctest_alias(
    test-remote-aur-cleanup-collector
    TESTS cpp.remote_aur_cleanup_collector
)

moguet_add_focused_ctest_alias(
    test-localization
    TESTS localization.contract
    TARGETS moguet_test_catalogs
)
moguet_add_focused_ctest_alias(
    test-runtime-identity
    TESTS cli.runtime_identity
    TARGETS moguet
)
moguet_add_focused_ctest_alias(test-app-config TESTS config.app)
moguet_add_focused_ctest_alias(test-user-config TESTS config.user)

moguet_add_focused_ctest_alias(
    test-reviewed-source-pinned-build
    TESTS
        cpp.reviewed_source_pinned_build
        build_contract.reviewed_source_authority_negative
)

set(_moguet_preflight_integration_tests "")
foreach(_moguet_preflight_case IN ITEMS
    simple
    repository-failure
    aur-failure
    relation-assessment
    relation-query-failure
)
    list(
        APPEND
        _moguet_preflight_integration_tests
        "cpp.aur_update_execution_preflight_integration.${_moguet_preflight_case}"
    )
endforeach()
moguet_add_focused_ctest_alias(
    test-aur-update-execution-preflight-integration
    TESTS ${_moguet_preflight_integration_tests}
)
moguet_add_focused_ctest_alias(
    test-aur-update-execution-preparation
    TESTS
        cpp.aur_update_execution_preparation
        cpp.aur_update_execution_preparation.integration
)

set(_moguet_repository_query_tests "")
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
    list(
        APPEND
        _moguet_repository_query_tests
        "cpp.repository_query.${_moguet_repository_query_case}"
    )
endforeach()
moguet_add_focused_ctest_alias(
    test-repository-query
    TESTS ${_moguet_repository_query_tests}
)

moguet_add_focused_ctest_alias(
    test-unified-plan-projection
    TESTS
        cpp.unified_plan_projection
        build_contract.unified_plan_projection_link_firewall
    TARGETS moguet_unified_plan_projection_object
)
moguet_add_focused_ctest_alias(
    test-projection-fixture-gate
    TESTS
        cpp.unified_plan_projection
        build_contract.unified_plan_projection_link_firewall
    TARGETS moguet_unified_plan_projection_object
)
moguet_add_focused_ctest_alias(
    test-unified-plan-renderer
    TESTS
        cpp.unified_plan_renderer
        build_contract.unified_plan_renderer_link_firewall
    TARGETS moguet_unified_plan_renderer_object
)
moguet_add_focused_ctest_alias(
    test-observation-contract-gate
    TESTS cpp.unified_plan_observation
)

moguet_add_focused_ctest_alias(
    test-aur-update-command
    TESTS cli.aur_update_command
)
moguet_add_focused_ctest_alias(
    test-upgrade-all-command
    TESTS cli.upgrade_all_command
    TARGETS moguet_catalogs
)
moguet_add_focused_ctest_alias(
    test-conflicts-replaces
    TESTS cli.conflicts_replaces
)
moguet_add_focused_ctest_alias(
    test-aur-rpc-validation
    TESTS cli.aur_rpc_validation
)
moguet_add_focused_ctest_alias(test-cli-parser TESTS cli.parser)
moguet_add_focused_ctest_alias(test-dry-run-command TESTS cli.dry_run)
moguet_add_focused_ctest_alias(
    test-commands-inspect
    TESTS cli.commands_inspect
    TARGETS moguet_catalogs
)
moguet_add_focused_ctest_alias(
    test-commands-source-maintenance
    TESTS
        cpp.process_stdin_fd
        cli.commands_source_maintenance
)
moguet_add_focused_ctest_alias(test-commands-sync TESTS cli.commands_sync)
moguet_add_focused_ctest_alias(test-pacman-routing TESTS cli.pacman_routing)
moguet_add_focused_ctest_alias(
    test-build-cache-symlink
    TESTS cli.build_cache_symlink
)
moguet_add_focused_ctest_alias(test-source-build TESTS cli.source_build)
moguet_add_focused_ctest_alias(test-source-selection TESTS cli.source_selection)
moguet_add_focused_ctest_alias(test-needed-contract TESTS cli.needed_contract)
moguet_add_focused_ctest_alias(test-pkgbuild-export TESTS cli.pkgbuild_export)

unset(_moguet_direct_focus)
unset(_moguet_direct_ctest)
unset(_moguet_preflight_case)
unset(_moguet_preflight_integration_tests)
unset(_moguet_repository_query_case)
unset(_moguet_repository_query_tests)
