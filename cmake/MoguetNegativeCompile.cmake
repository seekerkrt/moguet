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
message(
    STATUS
    "Reviewed source authority negative compile: baseline=1, "
    "expected diagnostics=${_moguet_authority_case_count}; "
    "Slice 4 narrow baselines=4, expected diagnostics=${_moguet_narrow_case_count}"
)
