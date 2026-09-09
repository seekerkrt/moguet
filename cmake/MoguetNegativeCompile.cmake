cmake_minimum_required(VERSION 3.18)

foreach(
    _moguet_required_variable
    IN ITEMS
        MOGUET_NEGATIVE_COMPILE_SOURCE
        MOGUET_NEGATIVE_COMPILE_CXX_FILE
        MOGUET_NEGATIVE_COMPILE_CXX_ARG1_FILE
        MOGUET_NEGATIVE_COMPILE_LAUNCHER_FILE
        MOGUET_NEGATIVE_COMPILE_CPPFLAGS_FILE
        MOGUET_NEGATIVE_COMPILE_CXXFLAGS_FILE
        MOGUET_NEGATIVE_COMPILE_CONFIGURATION_FLAGS_FILE
        MOGUET_NEGATIVE_COMPILE_PROJECT_OPTIONS_FILE
)
    if(NOT DEFINED ${_moguet_required_variable})
        message(
            FATAL_ERROR
            "Missing negative compile input: ${_moguet_required_variable}"
        )
    endif()
endforeach()

if(
    NOT EXISTS "${MOGUET_NEGATIVE_COMPILE_SOURCE}"
    OR IS_DIRECTORY "${MOGUET_NEGATIVE_COMPILE_SOURCE}"
)
    message(
        FATAL_ERROR
        "Negative compile source is unavailable: "
        "${MOGUET_NEGATIVE_COMPILE_SOURCE}"
    )
endif()

foreach(
    _moguet_input_file
    IN ITEMS
        "${MOGUET_NEGATIVE_COMPILE_CXX_FILE}"
        "${MOGUET_NEGATIVE_COMPILE_CXX_ARG1_FILE}"
        "${MOGUET_NEGATIVE_COMPILE_LAUNCHER_FILE}"
        "${MOGUET_NEGATIVE_COMPILE_CPPFLAGS_FILE}"
        "${MOGUET_NEGATIVE_COMPILE_CXXFLAGS_FILE}"
        "${MOGUET_NEGATIVE_COMPILE_CONFIGURATION_FLAGS_FILE}"
        "${MOGUET_NEGATIVE_COMPILE_PROJECT_OPTIONS_FILE}"
)
    if(NOT EXISTS "${_moguet_input_file}" OR IS_DIRECTORY "${_moguet_input_file}")
        message(
            FATAL_ERROR
            "Negative compile input file is unavailable: ${_moguet_input_file}"
        )
    endif()
endforeach()

file(READ "${MOGUET_NEGATIVE_COMPILE_CXX_FILE}" _moguet_cxx)
file(READ "${MOGUET_NEGATIVE_COMPILE_CXX_ARG1_FILE}" _moguet_cxx_arg1)
file(READ "${MOGUET_NEGATIVE_COMPILE_CPPFLAGS_FILE}" _moguet_cppflags_raw)
file(READ "${MOGUET_NEGATIVE_COMPILE_CXXFLAGS_FILE}" _moguet_cxxflags_raw)
file(
    READ
    "${MOGUET_NEGATIVE_COMPILE_CONFIGURATION_FLAGS_FILE}"
    _moguet_configuration_flags_raw
)
file(STRINGS "${MOGUET_NEGATIVE_COMPILE_LAUNCHER_FILE}" _moguet_launcher)
file(
    STRINGS
    "${MOGUET_NEGATIVE_COMPILE_PROJECT_OPTIONS_FILE}"
    _moguet_project_options
)

if(_moguet_cxx STREQUAL "")
    message(FATAL_ERROR "Negative compile C++ compiler is empty")
endif()

separate_arguments(_moguet_cxx_arg1 UNIX_COMMAND "${_moguet_cxx_arg1}")
separate_arguments(_moguet_cppflags UNIX_COMMAND "${_moguet_cppflags_raw}")
separate_arguments(_moguet_cxxflags UNIX_COMMAND "${_moguet_cxxflags_raw}")
separate_arguments(
    _moguet_configuration_flags
    UNIX_COMMAND
    "${_moguet_configuration_flags_raw}"
)

function(_moguet_append_arguments output_variable input_variable)
    set(_moguet_arguments "${${output_variable}}")
    foreach(_moguet_argument IN LISTS ${input_variable})
        # CMake list expansion uses semicolons as separators. Re-escape a
        # literal semicolon after parsing so execute_process receives the
        # original argument as one argv element.
        string(REPLACE ";" "\\;" _moguet_argument "${_moguet_argument}")
        list(APPEND _moguet_arguments "${_moguet_argument}")
    endforeach()
    set("${output_variable}" "${_moguet_arguments}" PARENT_SCOPE)
endfunction()

set(_moguet_compile_command "")
_moguet_append_arguments(_moguet_compile_command _moguet_launcher)
list(APPEND _moguet_compile_command "${_moguet_cxx}")
_moguet_append_arguments(_moguet_compile_command _moguet_cxx_arg1)

set(_moguet_common_arguments "")
_moguet_append_arguments(_moguet_common_arguments _moguet_cxxflags)
_moguet_append_arguments(
    _moguet_common_arguments
    _moguet_configuration_flags
)
_moguet_append_arguments(_moguet_common_arguments _moguet_project_options)
_moguet_append_arguments(_moguet_common_arguments _moguet_cppflags)

execute_process(
    COMMAND
        ${_moguet_compile_command}
        ${_moguet_common_arguments}
        -fsyntax-only
        "${MOGUET_NEGATIVE_COMPILE_SOURCE}"
    RESULT_VARIABLE _moguet_baseline_status
    OUTPUT_VARIABLE _moguet_baseline_stdout
    ERROR_VARIABLE _moguet_baseline_stderr
)
if(NOT "${_moguet_baseline_status}" STREQUAL "0")
    message(
        FATAL_ERROR
        "Reviewed source authority baseline compile failed with status "
        "${_moguet_baseline_status}\n"
        "${_moguet_baseline_stdout}${_moguet_baseline_stderr}"
    )
endif()

set(
    _moguet_authority_cases
    LIFECYCLE_EXPECTED
    FATAL_PREFLIGHT
    LIFECYCLE_ALREADY
    RETAINED_DESCRIPTOR
    ACCEPTED_CHECKOUT
    ALREADY_CHECKOUT
    PINNED_ACCEPTED
    PINNED_ALREADY
    EDITOR_BOUNDARY
    EDITOR_OVERLAY
    PROVENANCE_REVIEWED_GENERATION
    PROVENANCE_REVIEWED_BINDING
    PROVENANCE_REVIEWED_BINDING_AUTHORITY
    INSTALLED_ARTIFACT_BINDING
    PROVENANCE_PERSISTENT_DECODER
    INVOCATION_SOURCE_BUILD_CONTEXT
    REVIEWED_RECIPE_SNAPSHOT_IDENTITY
    INVOCATION_MAKEPKG_ENVIRONMENT
    EVALUATED_DEVEL_SOURCE_PROJECTION
    FRESH_DEVEL_PACKAGE_ARTIFACT
    EVALUATED_DEVEL_SOURCE_BUILD_PROOF
)
foreach(_moguet_authority_case IN LISTS _moguet_authority_cases)
    execute_process(
        COMMAND
            ${_moguet_compile_command}
            ${_moguet_common_arguments}
            "-DMOGUET_FORGE_${_moguet_authority_case}"
            -fsyntax-only
            "${MOGUET_NEGATIVE_COMPILE_SOURCE}"
        RESULT_VARIABLE _moguet_authority_status
        OUTPUT_VARIABLE _moguet_authority_stdout
        ERROR_VARIABLE _moguet_authority_stderr
    )
    set(
        _moguet_authority_diagnostic
        "${_moguet_authority_stdout}${_moguet_authority_stderr}"
    )
    if("${_moguet_authority_status}" STREQUAL "0")
        message(
            FATAL_ERROR
            "Reviewed source authority forgery ${_moguet_authority_case} "
            "compiled successfully"
        )
    endif()
    if(
        NOT _moguet_authority_diagnostic
            MATCHES "is private within this context"
    )
        message(
            FATAL_ERROR
            "Reviewed source authority forgery ${_moguet_authority_case} "
            "failed for an unexpected reason\n"
            "${_moguet_authority_diagnostic}"
        )
    endif()
endforeach()

list(LENGTH _moguet_authority_cases _moguet_authority_case_count)

# Slice 4 friendship must be closed even when only a narrow granting header
# is visible. Generated probes use no test/forge macros. Baselines prove the
# headers compile, and each negative requires redefinition or access control.
get_filename_component(_moguet_negative_state_dir
    "${MOGUET_NEGATIVE_COMPILE_PROJECT_OPTIONS_FILE}" DIRECTORY)
set(_moguet_narrow_case_count 0)
foreach(_moguet_header IN ITEMS
    devel_build_provenance
    invocation_owned_source_build_context
    artifact_archive_metadata
    evaluated_devel_source_build)
    set(_moguet_probe "${_moguet_negative_state_dir}/slice4-${_moguet_header}.cpp")
    set(_moguet_include "#include \"${_moguet_header}.hpp\"\n")
    file(WRITE "${_moguet_probe}" "${_moguet_include}int baseline() { return 0; }\n")
    execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments}
        -fsyntax-only "${_moguet_probe}"
        RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
    if(NOT "${_moguet_status}" STREQUAL "0")
        message(FATAL_ERROR "Narrow header ${_moguet_header} baseline failed: ${_moguet_stdout}${_moguet_stderr}")
    endif()
    foreach(_moguet_probe_kind IN ITEMS spoof raw_observation direct_constructor)
        if(_moguet_probe_kind STREQUAL "spoof")
            set(_moguet_body "class EvaluatedDevelSourceBuildAuthority {};\n")
            set(_moguet_expected "redefinition")
        elseif(_moguet_probe_kind STREQUAL "raw_observation")
            if(_moguet_header STREQUAL "artifact_archive_metadata")
                continue()
            endif()
            set(_moguet_body "MakepkgManagedGitWorkspaceRevisionObservation mint(UpstreamGitRevision a, UpstreamGitRevision b) { return MakepkgManagedGitWorkspaceRevisionObservation(a, b); }\n")
            set(_moguet_expected "is private within this context|private member|private constructor")
        else()
            if(_moguet_header STREQUAL "devel_build_provenance")
                # Its raw-observation case already exercises its direct constructor.
                continue()
            elseif(_moguet_header STREQUAL "artifact_archive_metadata")
                set(_moguet_body "void mint() { artifact_archive_metadata::RetainedDescriptorQueryAuthority authority(3); }\n")
            elseif(_moguet_header STREQUAL "invocation_owned_source_build_context")
                set(_moguet_body "void mint() { InvocationOwnedSourceBuildContext context(nullptr); }\n")
            else()
                set(_moguet_body "void mint() { EvaluatedDevelSourceBuildProof proof(nullptr); }\n")
            endif()
            set(_moguet_expected "is private within this context|private member|private constructor")
        endif()
        file(WRITE "${_moguet_probe}" "${_moguet_include}${_moguet_body}")
        execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments}
            -fsyntax-only "${_moguet_probe}"
            RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
        if("${_moguet_status}" STREQUAL "0" OR
            NOT "${_moguet_stdout}${_moguet_stderr}" MATCHES "${_moguet_expected}")
            message(FATAL_ERROR "Narrow authority ${_moguet_header}/${_moguet_probe_kind} failed its expected rejection: status=${_moguet_status}\n${_moguet_stdout}${_moguet_stderr}")
        endif()
        file(WRITE "${_moguet_negative_state_dir}/slice4-${_moguet_header}-${_moguet_probe_kind}.diagnostic.txt"
            "${_moguet_stdout}${_moguet_stderr}")
        math(EXPR _moguet_narrow_case_count "${_moguet_narrow_case_count} + 1")
    endforeach()
endforeach()
# S5-A: the decoder authority is complete even without the codec header.
set(_moguet_s5_case_count 0)
foreach(_moguet_header IN ITEMS installed_artifact_binding devel_build_provenance evaluated_devel_source_build evaluated_devel_source_artifact_transport)
    set(_moguet_probe "${_moguet_negative_state_dir}/slice5-${_moguet_header}.cpp")
    set(_moguet_include "#include \"${_moguet_header}.hpp\"\n")
    file(WRITE "${_moguet_probe}" "${_moguet_include}int baseline() { return 0; }\n")
    execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments}
        -fsyntax-only "${_moguet_probe}"
        RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
    if(NOT "${_moguet_status}" STREQUAL "0")
        message(FATAL_ERROR "Slice 5 narrow baseline ${_moguet_header} failed: ${_moguet_stdout}${_moguet_stderr}")
    endif()
    if(_moguet_header MATCHES "^(installed_artifact_binding|devel_build_provenance)$")
        set(_moguet_cases spoof generation binding decoder)
    else()
        set(_moguet_cases spoof transport)
    endif()
    foreach(_moguet_case IN LISTS _moguet_cases)
        set(_moguet_expected "is private within this context|private member|private constructor")
        if(_moguet_case STREQUAL "spoof")
            if(_moguet_header MATCHES "^(installed_artifact_binding|devel_build_provenance)$")
                set(_moguet_body "class DevelBuildProvenancePersistentDecoderAccess { public: static InstalledPackageRecordGeneration forge() { return InstalledPackageRecordGeneration(InstalledPackageRecordGenerationScheme::LinuxNameToHandleAt, \"raw\"); } static InstalledArtifactBinding forge(PackageChildIdentity p, PackageVersionIdentity v, InstalledPackageArchitectureIdentity a, AlpmMtreeSha256Digest m, InstalledDatabaseRecordSha256Digest d, InstalledPackageRecordGeneration g) { return InstalledArtifactBinding::make(p, v, a, m, d, g); } };\n")
            else()
                set(_moguet_body "class EvaluatedDevelSourceArtifactTransport {};\n")
            endif()
            set(_moguet_expected "redefinition")
        elseif(_moguet_case STREQUAL "generation")
            set(_moguet_body "auto forge() { return InstalledPackageRecordGeneration(InstalledPackageRecordGenerationScheme::LinuxNameToHandleAt, \"raw\"); }\n")
        elseif(_moguet_case STREQUAL "binding")
            set(_moguet_body "auto forge(PackageChildIdentity p, PackageVersionIdentity v, InstalledPackageArchitectureIdentity a, AlpmMtreeSha256Digest m, InstalledDatabaseRecordSha256Digest d, InstalledPackageRecordGeneration g) { return InstalledArtifactBinding::make(p, v, a, m, d, g); }\n")
        elseif(_moguet_case STREQUAL "decoder")
            set(_moguet_body "auto forge() { return &DevelBuildProvenancePersistentDecoderAccess::decode_document; }\n")
        else()
            set(_moguet_body "void forge() { EvaluatedDevelSourceArtifactTransport value(nullptr); }\n")
        endif()
        file(WRITE "${_moguet_probe}" "${_moguet_include}${_moguet_body}")
        execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments}
            -fsyntax-only "${_moguet_probe}"
            RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
        if("${_moguet_status}" STREQUAL "0" OR
            NOT "${_moguet_stdout}${_moguet_stderr}" MATCHES "${_moguet_expected}")
            message(FATAL_ERROR "Slice 5 narrow ${_moguet_header}/${_moguet_case} failed expected rejection: status=${_moguet_status}\n${_moguet_stdout}${_moguet_stderr}")
        endif()
        file(WRITE "${_moguet_negative_state_dir}/slice5-${_moguet_header}-${_moguet_case}.diagnostic.txt"
            "${_moguet_stdout}${_moguet_stderr}")
        math(EXPR _moguet_s5_case_count "${_moguet_s5_case_count} + 1")
    endforeach()
endforeach()
message(STATUS "Slice 5 narrow baselines=4, expected diagnostics=${_moguet_s5_case_count}")

# S5-B live authority must remain closed from every granting narrow header.
set(_moguet_s5b_case_count 0)
foreach(_moguet_header IN ITEMS installed_artifact_binding fresh_installed_artifact_binding installed_artifact_binding_observer_authority exact_artifact_transaction_receipt)
    set(_moguet_probe "${_moguet_negative_state_dir}/slice5b-${_moguet_header}.cpp")
    file(WRITE "${_moguet_probe}" "#include \"${_moguet_header}.hpp\"\nint baseline() { return 0; }\n")
    execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments} -fsyntax-only "${_moguet_probe}"
        RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
    if(NOT "${_moguet_status}" STREQUAL "0")
        message(FATAL_ERROR "Slice 5-B narrow baseline ${_moguet_header} failed: ${_moguet_stdout}${_moguet_stderr}")
    endif()
    if(NOT _moguet_header STREQUAL "exact_artifact_transaction_receipt")
        file(WRITE "${_moguet_probe}" "#include \"${_moguet_header}.hpp\"\nclass InstalledArtifactBindingObserver {};\n")
        execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments} -fsyntax-only "${_moguet_probe}"
            RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
        if("${_moguet_status}" STREQUAL "0" OR NOT "${_moguet_stdout}${_moguet_stderr}" MATCHES "redefinition")
            message(FATAL_ERROR "Slice 5-B ${_moguet_header} accepted same-name observer spoof: ${_moguet_stdout}${_moguet_stderr}")
        endif()
        file(WRITE "${_moguet_negative_state_dir}/slice5b-${_moguet_header}-spoof.diagnostic.txt" "${_moguet_stdout}${_moguet_stderr}")
        math(EXPR _moguet_s5b_case_count "${_moguet_s5b_case_count} + 1")
    endif()
endforeach()
set(_moguet_s5b_include "#include \"fresh_installed_artifact_binding.hpp\"\n#include \"exact_artifact_transaction_receipt.hpp\"\n#include \"devel_build_provenance_codec.hpp\"\n")
foreach(_moguet_case IN ITEMS generation binding decoded path fd tuple observer receipt)
    set(_moguet_expected "is private within this context|private member|private constructor")
    if(_moguet_case STREQUAL "generation")
        set(_moguet_body "auto forge() { return InstalledPackageRecordGeneration(InstalledPackageRecordGenerationScheme::LinuxNameToHandleAt, \"raw\"); }")
    elseif(_moguet_case STREQUAL "binding")
        set(_moguet_body "auto forge(PackageChildIdentity p, PackageVersionIdentity v, InstalledPackageArchitectureIdentity a, AlpmMtreeSha256Digest m, InstalledDatabaseRecordSha256Digest d, InstalledPackageRecordGeneration g) { return InstalledArtifactBinding::make(p, v, a, m, d, g); }")
    elseif(_moguet_case STREQUAL "decoded")
        set(_moguet_body "auto forge(const DevelBuildProvenanceDecoded& historical) { return FreshInstalledArtifactBinding(historical.provenance.installed_binding(), \"token\", 0, {}, {}, {}); }")
    elseif(_moguet_case STREQUAL "observer")
        set(_moguet_body "auto forge() { return &InstalledArtifactBindingObserver::observe; }")
    elseif(_moguet_case STREQUAL "receipt")
        set(_moguet_body "auto forge(SourceArtifactInstallRootPrepareRequest m, ExactArtifactOperationRecords r, ExactArtifactRootEvidence e) { return ExactArtifactTransactionReceipt(m, \"stage\", r, e, {}, {}); }")
    else()
        set(_moguet_expected "no matching|no viable|cannot convert|invalid conversion|is private|private member")
        if(_moguet_case STREQUAL "path")
            set(_moguet_body "auto forge() { return InstalledArtifactBindingObserver::observe(\"/var/lib/pacman\", \"package\"); }")
        elseif(_moguet_case STREQUAL "fd")
            set(_moguet_body "auto forge(int fd) { return InstalledArtifactBindingObserver::observe(fd, \"package\"); }")
        else()
            set(_moguet_body "auto forge() { return InstalledArtifactBindingObserver::observe(\"path\", \"name\", \"version\", \"digest\", \"generation\"); }")
        endif()
    endif()
    set(_moguet_probe "${_moguet_negative_state_dir}/slice5b-${_moguet_case}.cpp")
    file(WRITE "${_moguet_probe}" "${_moguet_s5b_include}${_moguet_body}\n")
    execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments} -fsyntax-only "${_moguet_probe}"
        RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
    if("${_moguet_status}" STREQUAL "0" OR NOT "${_moguet_stdout}${_moguet_stderr}" MATCHES "${_moguet_expected}")
        message(FATAL_ERROR "Slice 5-B ${_moguet_case} failed expected rejection: ${_moguet_status}\n${_moguet_stdout}${_moguet_stderr}")
    endif()
    file(WRITE "${_moguet_negative_state_dir}/slice5b-${_moguet_case}.diagnostic.txt" "${_moguet_stdout}${_moguet_stderr}")
    math(EXPR _moguet_s5b_case_count "${_moguet_s5b_case_count} + 1")
endforeach()
message(STATUS "Slice 5-B narrow baselines=4, expected diagnostics=${_moguet_s5b_case_count}")

# Final construction and the live aggregate share the complete private owner.
set(_moguet_s5c_case_count 0)
foreach(_moguet_header IN ITEMS devel_source_artifact_install devel_source_artifact_install_authority
    evaluated_devel_source_build evaluated_devel_source_artifact_transport
    exact_artifact_transaction_receipt fresh_installed_artifact_binding)
    set(_moguet_probe "${_moguet_negative_state_dir}/slice5c-${_moguet_header}.cpp")
    file(WRITE "${_moguet_probe}" "#include \"${_moguet_header}.hpp\"\nint baseline() { return 0; }\n")
    execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments} -fsyntax-only "${_moguet_probe}"
        RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
    if(NOT "${_moguet_status}" STREQUAL "0")
        message(FATAL_ERROR "S5-C narrow baseline ${_moguet_header} failed: ${_moguet_stdout}${_moguet_stderr}")
    endif()
    file(WRITE "${_moguet_probe}" "#include \"${_moguet_header}.hpp\"\nclass DevelSourceArtifactInstallAuthority {};\n")
    execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments} -fsyntax-only "${_moguet_probe}"
        RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
    if("${_moguet_status}" STREQUAL "0" OR NOT "${_moguet_stdout}${_moguet_stderr}" MATCHES "redefinition")
        message(FATAL_ERROR "S5-C narrow authority spoof accepted: ${_moguet_header}: ${_moguet_stdout}${_moguet_stderr}")
    endif()
    file(WRITE "${_moguet_negative_state_dir}/slice5c-${_moguet_header}-spoof.diagnostic.txt" "${_moguet_stdout}${_moguet_stderr}")
    math(EXPR _moguet_s5c_case_count "${_moguet_s5c_case_count} + 1")
endforeach()
set(_moguet_s5c_include "#include \"devel_source_artifact_install.hpp\"\n#include \"evaluated_devel_source_build.hpp\"\n#include \"exact_artifact_transaction_receipt.hpp\"\n#include \"fresh_installed_artifact_binding.hpp\"\n#include <type_traits>\n#include <utility>\n")
set(_moguet_probe "${_moguet_negative_state_dir}/slice5c-positive.cpp")
file(WRITE "${_moguet_probe}" "${_moguet_s5c_include}static_assert(std::is_nothrow_move_constructible_v<InstalledDevelSourceBuildProof>);\nstatic_assert(std::is_nothrow_move_constructible_v<DevelSourceArtifactInstallResult>);\nauto finish(EvaluatedDevelSourceArtifactTransport& t) { return t.finalize(); }\n")
execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments} -fsyntax-only "${_moguet_probe}"
    RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
if(NOT "${_moguet_status}" STREQUAL "0")
    message(FATAL_ERROR "S5-C positive baseline failed: ${_moguet_stdout}${_moguet_stderr}")
endif()
foreach(_moguet_case IN ITEMS direct default copy built_tuple raw_receipt persistent fresh_without_receipt
    receipt_decoded raw_factory private_factory aggregate_raw aggregate_contradiction component_tuple)
    set(_moguet_expected "no matching|no viable|cannot convert|invalid conversion|is private|private member|private constructor|deleted")
    if(_moguet_case STREQUAL "direct")
        set(_moguet_body "auto forge() { return InstalledDevelSourceBuildProof(nullptr); }")
    elseif(_moguet_case STREQUAL "default")
        set(_moguet_body "InstalledDevelSourceBuildProof forge;")
    elseif(_moguet_case STREQUAL "copy")
        set(_moguet_body "auto forge(const InstalledDevelSourceBuildProof& p) { return p; }")
    elseif(_moguet_case STREQUAL "built_tuple")
        set(_moguet_body "auto forge(EvaluatedDevelSourceBuildProof p) { return InstalledDevelSourceBuildProof(std::move(p), \"name\", \"version\", \"digest\"); }")
    elseif(_moguet_case STREQUAL "raw_receipt")
        set(_moguet_body "auto forge(ExactArtifactRootEvidence e) { return InstalledDevelSourceBuildProof(std::move(e)); }")
    elseif(_moguet_case STREQUAL "persistent")
        set(_moguet_body "auto forge(InstalledArtifactBinding b) { return InstalledDevelSourceBuildProof(std::move(b)); }")
    elseif(_moguet_case STREQUAL "fresh_without_receipt")
        set(_moguet_body "auto forge(FreshInstalledArtifactBinding b) { return InstalledDevelSourceBuildProof(std::move(b)); }")
    elseif(_moguet_case STREQUAL "receipt_decoded")
        set(_moguet_body "auto forge(ExactArtifactTransactionReceipt r, InstalledArtifactBinding b) { return InstalledDevelSourceBuildProof(std::move(r), std::move(b)); }")
    elseif(_moguet_case STREQUAL "raw_factory")
        set(_moguet_body "auto forge() { return DevelSourceArtifactInstallAuthority::finalize(\"token\", \"path\", \"digest\"); }")
    elseif(_moguet_case STREQUAL "private_factory")
        set(_moguet_body "auto forge() { return &DevelSourceArtifactInstallAuthority::finalize; }")
    elseif(_moguet_case STREQUAL "aggregate_raw")
        set(_moguet_body "auto forge(InstalledArtifactBinding b) { return DevelSourceArtifactInstallResult(std::move(b)); }")
    elseif(_moguet_case STREQUAL "aggregate_contradiction")
        set(_moguet_body "auto forge(InstalledDevelSourceBuildProof p) { return DevelSourceArtifactInstallResult(DevelSourceArtifactInstallOperation::Failed, std::move(p)); }")
    else()
        set(_moguet_body "auto forge(EvaluatedDevelSourceBuildProof p, ExactArtifactTransactionReceipt r, FreshInstalledArtifactBinding b) { return InstalledDevelSourceBuildProof(std::move(p), std::move(r), std::move(b)); }")
    endif()
    set(_moguet_probe "${_moguet_negative_state_dir}/slice5c-${_moguet_case}.cpp")
    file(WRITE "${_moguet_probe}" "${_moguet_s5c_include}${_moguet_body}\n")
    execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments} -fsyntax-only "${_moguet_probe}"
        RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
    if("${_moguet_status}" STREQUAL "0" OR NOT "${_moguet_stdout}${_moguet_stderr}" MATCHES "${_moguet_expected}")
        message(FATAL_ERROR "S5-C ${_moguet_case} failed expected rejection: ${_moguet_status}\n${_moguet_stdout}${_moguet_stderr}")
    endif()
    file(WRITE "${_moguet_negative_state_dir}/slice5c-${_moguet_case}.diagnostic.txt" "${_moguet_stdout}${_moguet_stderr}")
    math(EXPR _moguet_s5c_case_count "${_moguet_s5c_case_count} + 1")
endforeach()
# Do not pull in the transport header as a control here: each granting narrow
# header must itself close the reverse friend edge to the actual transport.
foreach(_moguet_header IN ITEMS devel_source_artifact_install_authority devel_source_artifact_install)
    foreach(_moguet_entry IN ITEMS finalize correlate)
        set(_moguet_probe "${_moguet_negative_state_dir}/slice5c-${_moguet_header}-inverse-${_moguet_entry}.cpp")
        file(WRITE "${_moguet_probe}"
            "#include \"${_moguet_header}.hpp\"\nclass EvaluatedDevelSourceArtifactTransport { public: static auto expose() { return &DevelSourceArtifactInstallAuthority::${_moguet_entry}; } };\nauto escaped = EvaluatedDevelSourceArtifactTransport::expose();\n")
        execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments} -fsyntax-only "${_moguet_probe}"
            RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
        file(WRITE "${_moguet_negative_state_dir}/slice5c-${_moguet_header}-inverse-${_moguet_entry}.diagnostic.txt"
            "${_moguet_stdout}${_moguet_stderr}")
        if("${_moguet_status}" STREQUAL "0" OR NOT "${_moguet_stdout}${_moguet_stderr}" MATCHES "redefinition")
            message(FATAL_ERROR "S5-C narrow inverse friend ${_moguet_header}/${_moguet_entry} failed expected rejection: ${_moguet_status}\n${_moguet_stdout}${_moguet_stderr}")
        endif()
        math(EXPR _moguet_s5c_case_count "${_moguet_s5c_case_count} + 1")
    endforeach()
endforeach()
message(STATUS "Slice 5-C narrow baselines=6, positive=1, expected diagnostics=${_moguet_s5c_case_count}")
message(
    STATUS
    "Reviewed source authority negative compile: baseline=1, "
    "expected diagnostics=${_moguet_authority_case_count}; "
    "Slice 4 narrow baselines=4, expected diagnostics=${_moguet_narrow_case_count}"
)


# S6-B: historical values never construct the trusted live publication result.
set(_moguet_s6b_count 0)
foreach(_moguet_header IN ITEMS devel_build_provenance_publication_authority devel_build_provenance_publication)
    set(_moguet_probe "${_moguet_negative_state_dir}/slice6b-${_moguet_header}.cpp")
    file(WRITE "${_moguet_probe}" "#include \"${_moguet_header}.hpp\"\nint baseline() { return sizeof(DevelBuildProvenancePublicationAuthority); }\n")
    execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments} -fsyntax-only "${_moguet_probe}"
        RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
    if(NOT "${_moguet_status}" STREQUAL "0")
        message(FATAL_ERROR "S6-B narrow baseline failed: ${_moguet_header}: ${_moguet_stdout}${_moguet_stderr}")
    endif()
    file(WRITE "${_moguet_probe}" "#include \"${_moguet_header}.hpp\"\nclass DevelBuildProvenancePublicationAuthority {};\n")
    execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments} -fsyntax-only "${_moguet_probe}"
        RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
    if("${_moguet_status}" STREQUAL "0" OR NOT "${_moguet_stdout}${_moguet_stderr}" MATCHES "redefinition")
        message(FATAL_ERROR "S6-B narrow authority spoof accepted: ${_moguet_header}")
    endif()
    file(WRITE "${_moguet_negative_state_dir}/slice6b-${_moguet_header}-spoof.diagnostic.txt" "${_moguet_stdout}${_moguet_stderr}")
    math(EXPR _moguet_s6b_count "${_moguet_s6b_count} + 1")
endforeach()
set(_moguet_s6b_include "#include \"devel_build_provenance_publication.hpp\"\n#include <utility>\n#include <type_traits>\n")
file(WRITE "${_moguet_negative_state_dir}/slice6b-positive.cpp"
    "${_moguet_s6b_include}static_assert(std::is_nothrow_move_constructible_v<DevelBuildProvenancePublicationResult>);\nauto run(DevelSourceArtifactInstallResult r) { return publish_installed_devel_source_build(std::move(r)); }\n")
execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments} -fsyntax-only "${_moguet_negative_state_dir}/slice6b-positive.cpp"
    RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
if(NOT "${_moguet_status}" STREQUAL "0")
    message(FATAL_ERROR "S6-B positive baseline failed: ${_moguet_stdout}${_moguet_stderr}")
endif()
foreach(_moguet_case IN ITEMS raw decoded binding generation store_dto identity direct private_entry default copy assign contradiction const_proof extract mutable_state namespace inheritance reverse_result reverse_install late)
    set(_moguet_include "${_moguet_s6b_include}")
    set(_moguet_expected "no matching|no viable|cannot convert|could not convert|invalid conversion|is private|private member|private constructor|deleted|final|redefinition")
    if(_moguet_case STREQUAL "raw")
        set(_moguet_body "auto forge(DevelBuildProvenance p) { return publish_installed_devel_source_build(std::move(p)); }")
    elseif(_moguet_case STREQUAL "decoded")
        set(_moguet_body "auto forge(DevelBuildProvenanceDecoded p) { return DevelBuildProvenancePublicationResult(std::move(p.provenance)); }")
    elseif(_moguet_case STREQUAL "binding")
        set(_moguet_body "auto forge(InstalledArtifactBinding p) { return publish_installed_devel_source_build(std::move(p)); }")
    elseif(_moguet_case STREQUAL "generation")
        set(_moguet_body "auto forge() { return DevelBuildProvenancePublicationResult(1, \"digest\", \"path\"); }")
    elseif(_moguet_case STREQUAL "store_dto")
        set(_moguet_body "auto forge(DevelBuildProvenanceStorePublished p) { return DevelBuildProvenancePublicationResult(std::move(p)); }")
    elseif(_moguet_case STREQUAL "identity")
        set(_moguet_body "auto forge(DevelBuildProvenancePublicationIdentity p) { return DevelBuildProvenancePublicationResult(std::move(p)); }")
    elseif(_moguet_case STREQUAL "direct")
        set(_moguet_body "auto forge(DevelSourceArtifactInstallResult p) { return DevelBuildProvenancePublicationResult(std::move(p)); }")
    elseif(_moguet_case STREQUAL "private_entry")
        set(_moguet_body "auto forge() { return &DevelBuildProvenancePublicationAuthority::publish; }")
    elseif(_moguet_case STREQUAL "default")
        set(_moguet_body "DevelBuildProvenancePublicationResult forged;")
    elseif(_moguet_case STREQUAL "copy")
        set(_moguet_body "auto forge(const DevelBuildProvenancePublicationResult& p) { return p; }")
    elseif(_moguet_case STREQUAL "assign")
        set(_moguet_body "void forge(DevelBuildProvenancePublicationResult& a, DevelBuildProvenancePublicationResult& b) { a = std::move(b); }")
    elseif(_moguet_case STREQUAL "contradiction")
        set(_moguet_body "auto forge(DevelSourceArtifactInstallResult p) { return DevelBuildProvenancePublicationResult(std::move(p), DevelBuildProvenancePublicationState::Complete); }")
    elseif(_moguet_case STREQUAL "const_proof")
        set(_moguet_body "auto forge(const InstalledDevelSourceBuildProof& p) { return publish_installed_devel_source_build(p); }")
    elseif(_moguet_case STREQUAL "extract")
        set(_moguet_body "auto forge(DevelBuildProvenancePublicationResult& p) { return publish_installed_devel_source_build(std::move(p.installation())); }")
    elseif(_moguet_case STREQUAL "mutable_state")
        set(_moguet_body "void forge(DevelBuildProvenancePublicationResult& p) { p.state_ = DevelBuildProvenancePublicationState::Complete; }")
    elseif(_moguet_case STREQUAL "namespace")
        set(_moguet_body "namespace fake { class DevelBuildProvenancePublicationAuthority { static auto forge() { return &::DevelBuildProvenancePublicationAuthority::publish; } }; }")
    elseif(_moguet_case STREQUAL "inheritance")
        set(_moguet_body "class Forged : public DevelBuildProvenancePublicationAuthority {};")
    elseif(_moguet_case MATCHES "^reverse_")
        set(_moguet_include "#include \"devel_build_provenance_publication_authority.hpp\"\n")
        if(_moguet_case STREQUAL "reverse_result")
            set(_moguet_friend DevelBuildProvenancePublicationResult)
        else()
            set(_moguet_friend DevelSourceArtifactInstallResult)
        endif()
        set(_moguet_body "class ${_moguet_friend} { static auto expose() { return &DevelBuildProvenancePublicationAuthority::publish; } };")
        set(_moguet_expected "is private|private member")
    else()
        set(_moguet_include "#include \"devel_build_provenance_publication_authority.hpp\"\n")
        set(_moguet_body "class DevelBuildProvenancePublicationResult {};\n#include \"devel_build_provenance_publication.hpp\"\n")
        set(_moguet_expected "redefinition")
    endif()
    set(_moguet_probe "${_moguet_negative_state_dir}/slice6b-${_moguet_case}.cpp")
    file(WRITE "${_moguet_probe}" "${_moguet_include}${_moguet_body}\n")
    execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments} -fsyntax-only "${_moguet_probe}"
        RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
    file(WRITE "${_moguet_negative_state_dir}/slice6b-${_moguet_case}.diagnostic.txt" "${_moguet_stdout}${_moguet_stderr}")
    if("${_moguet_status}" STREQUAL "0" OR NOT "${_moguet_stdout}${_moguet_stderr}" MATCHES "${_moguet_expected}")
        message(FATAL_ERROR "S6-B ${_moguet_case} failed expected rejection: ${_moguet_status}\n${_moguet_stdout}${_moguet_stderr}")
    endif()
    math(EXPR _moguet_s6b_count "${_moguet_s6b_count} + 1")
endforeach()
message(STATUS "Slice 6-B narrow baselines=2, positive=1, expected diagnostics=${_moguet_s6b_count}")

# S7-A: current I/O is a separate closed producer; historical values cannot
# construct an observed result or regain S5 transaction authority.
set(_moguet_s7a_count 0)
foreach(_moguet_header IN ITEMS current_installed_artifact_binding_observer_authority current_installed_artifact_binding_observer installed_artifact_binding)
    set(_moguet_probe "${_moguet_negative_state_dir}/slice7a-${_moguet_header}.cpp")
    file(WRITE "${_moguet_probe}" "#include \"${_moguet_header}.hpp\"\nint baseline() { return sizeof(CurrentInstalledArtifactBindingObserver); }\n")
    execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments} -fsyntax-only "${_moguet_probe}"
        RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
    if(NOT "${_moguet_status}" STREQUAL "0")
        message(FATAL_ERROR "S7-A narrow baseline failed: ${_moguet_header}: ${_moguet_stdout}${_moguet_stderr}")
    endif()
    file(WRITE "${_moguet_probe}" "#include \"${_moguet_header}.hpp\"\nclass CurrentInstalledArtifactBindingObserver {};\n")
    execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments} -fsyntax-only "${_moguet_probe}"
        RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
    file(WRITE "${_moguet_negative_state_dir}/slice7a-${_moguet_header}-spoof.diagnostic.txt" "${_moguet_stdout}${_moguet_stderr}")
    if("${_moguet_status}" STREQUAL "0" OR NOT "${_moguet_stdout}${_moguet_stderr}" MATCHES "redefinition")
        message(FATAL_ERROR "S7-A same-name owner accepted: ${_moguet_header}")
    endif()
    math(EXPR _moguet_s7a_count "${_moguet_s7a_count} + 1")
endforeach()
set(_moguet_s7a_include "#include \"current_installed_artifact_binding_observer.hpp\"\n#include \"devel_build_provenance_publication.hpp\"\n#include \"fresh_installed_artifact_binding.hpp\"\n#include \"devel_git_revision_comparison.hpp\"\n#include <type_traits>\n#include <utility>\n")
file(WRITE "${_moguet_negative_state_dir}/slice7a-positive.cpp" "${_moguet_s7a_include}static_assert(!std::is_default_constructible_v<CurrentInstalledArtifactBindingObserved>);\nstatic_assert(std::is_copy_constructible_v<CurrentInstalledArtifactBindingObserved>);\nstatic_assert(std::is_copy_assignable_v<CurrentInstalledArtifactBindingObserved>);\nstatic_assert(std::is_nothrow_move_constructible_v<CurrentInstalledArtifactBindingObserved>);\nstatic_assert(std::is_move_assignable_v<CurrentInstalledArtifactBindingObserved>);\nstatic_assert(std::is_same_v<decltype(std::declval<CurrentInstalledArtifactBindingObserved&>().binding()), const InstalledArtifactBinding&>);\nauto observe(const PackageChildIdentity& p) { return observe_current_installed_artifact_binding(p); }\nauto compare(const UpstreamGitRevision& a, const UpstreamGitRevision& b) { return compare_devel_git_revision(a,b); }\n")
execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments} -fsyntax-only "${_moguet_negative_state_dir}/slice7a-positive.cpp"
    RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
if(NOT "${_moguet_status}" STREQUAL "0")
    message(FATAL_ERROR "S7-A positive failed: ${_moguet_stdout}${_moguet_stderr}")
endif()
foreach(_moguet_case IN ITEMS raw_snapshot raw_generation raw_tuple raw_binding decoded private_entry reverse_observed reverse_binding late namespace inheritance default to_fresh to_proof to_publication raw_git)
    set(_moguet_include "${_moguet_s7a_include}")
    set(_moguet_expected "no matching|no viable|cannot convert|could not convert|invalid conversion|invalid initialization|too many arguments|is private|private member|private constructor|deleted|final|redefinition")
    if(_moguet_case STREQUAL "raw_snapshot")
        set(_moguet_body "auto forge(InstalledPackageRecordObservation p) { return observe_current_installed_artifact_binding(p); }")
    elseif(_moguet_case STREQUAL "raw_generation")
        set(_moguet_body "auto forge(InstalledPackageRecordGeneration p) { return CurrentInstalledArtifactBindingObserved(p); }")
    elseif(_moguet_case STREQUAL "raw_tuple")
        set(_moguet_body "auto forge(PackageChildIdentity p) { return observe_current_installed_artifact_binding(p, \"path\", \"generation\", \"digest\", \"mtree\"); }")
    elseif(_moguet_case STREQUAL "raw_binding")
        set(_moguet_body "auto forge(InstalledArtifactBinding p) { return CurrentInstalledArtifactBindingObservation{p}; }")
    elseif(_moguet_case STREQUAL "decoded")
        set(_moguet_body "auto forge(DevelBuildProvenanceDecoded p, InstalledDatabaseWorld w) { return CurrentInstalledArtifactBindingObserved(p.provenance.installed_binding(),w); }")
    elseif(_moguet_case STREQUAL "private_entry")
        set(_moguet_body "auto forge() { return &CurrentInstalledArtifactBindingObserver::observe; }")
    elseif(_moguet_case MATCHES "^reverse_")
        set(_moguet_include "#include \"current_installed_artifact_binding_observer_authority.hpp\"\n")
        if(_moguet_case STREQUAL "reverse_observed")
            set(_moguet_fake CurrentInstalledArtifactBindingObserved)
        else()
            set(_moguet_fake InstalledArtifactBinding)
        endif()
        set(_moguet_body "class ${_moguet_fake} { static auto forge() { return &CurrentInstalledArtifactBindingObserver::observe; } };")
        set(_moguet_expected "is private|private member")
    elseif(_moguet_case STREQUAL "late")
        set(_moguet_include "#include \"current_installed_artifact_binding_observer_authority.hpp\"\n")
        set(_moguet_body "class CurrentInstalledArtifactBindingObserved {};\n#include \"current_installed_artifact_binding_observer.hpp\"")
        set(_moguet_expected "redefinition")
    elseif(_moguet_case STREQUAL "namespace")
        set(_moguet_body "namespace fake { class CurrentInstalledArtifactBindingObserver { static auto forge() { return &::InstalledArtifactBinding::make; } }; }")
    elseif(_moguet_case STREQUAL "inheritance")
        set(_moguet_body "class Fake : public CurrentInstalledArtifactBindingObserver {};")
    elseif(_moguet_case STREQUAL "default")
        set(_moguet_body "CurrentInstalledArtifactBindingObserved forged;")
    elseif(_moguet_case STREQUAL "to_fresh")
        set(_moguet_body "auto forge(CurrentInstalledArtifactBindingObserved p) { return FreshInstalledArtifactBinding(p.binding()); }")
    elseif(_moguet_case STREQUAL "to_proof")
        set(_moguet_body "auto forge(CurrentInstalledArtifactBindingObserved p) { return InstalledDevelSourceBuildProof(p); }")
    elseif(_moguet_case STREQUAL "to_publication")
        set(_moguet_body "auto forge(CurrentInstalledArtifactBindingObserved p) { return publish_installed_devel_source_build(p); }")
    else()
        set(_moguet_body "auto forge() { return compare_devel_git_revision(\"a\", \"b\"); }")
    endif()
    set(_moguet_probe "${_moguet_negative_state_dir}/slice7a-${_moguet_case}.cpp")
    file(WRITE "${_moguet_probe}" "${_moguet_include}${_moguet_body}\n")
    execute_process(COMMAND ${_moguet_compile_command} ${_moguet_common_arguments} -fsyntax-only "${_moguet_probe}"
        RESULT_VARIABLE _moguet_status OUTPUT_VARIABLE _moguet_stdout ERROR_VARIABLE _moguet_stderr)
    file(WRITE "${_moguet_negative_state_dir}/slice7a-${_moguet_case}.diagnostic.txt" "${_moguet_stdout}${_moguet_stderr}")
    if("${_moguet_status}" STREQUAL "0" OR NOT "${_moguet_stdout}${_moguet_stderr}" MATCHES "${_moguet_expected}")
        message(FATAL_ERROR "S7-A ${_moguet_case} failed expected rejection: ${_moguet_status}\n${_moguet_stdout}${_moguet_stderr}")
    endif()
    math(EXPR _moguet_s7a_count "${_moguet_s7a_count} + 1")
endforeach()
message(STATUS "Slice 7-A narrow baselines=3, positive=1, expected diagnostics=${_moguet_s7a_count}")
