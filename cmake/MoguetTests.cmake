# CMake owns the C++ test compile/link graph and CTest owns its runtime
# registration.  Keep the inventory checks in this module so adding a target,
# stub, or firewall cannot silently leave the corresponding authority ledger.

include(CMakeParseArguments)

set(MOGUET_CPP_TEST_TARGETS "")
set(MOGUET_CPP_TEST_SUPPORT_SOURCES "")
set(MOGUET_CPP_TEST_FIREWALL_TARGETS "")
set(MOGUET_CPP_TEST_FIREWALL_DESCRIPTORS "")
set(MOGUET_CPP_TEST_OBJECT_TARGETS "")
set(MOGUET_CTEST_RUNTIME_TARGETS "")
set(MOGUET_CTEST_NAMES "")

# PkgConfig::ALPM carries both compile usage requirements and the real link.
# Wrap only the latter so tests such as the root-identity executable can link
# real libalpm without leaking its compile profile into a replacement OBJECT.
# LINK_ONLY has been available for INTERFACE_LINK_LIBRARIES since CMake 3.1.
add_library(moguet_test_real_alpm_link_contract INTERFACE)
target_link_libraries(
    moguet_test_real_alpm_link_contract
    INTERFACE "$<LINK_ONLY:PkgConfig::ALPM>"
)
add_library(moguet_test_real_curl_link_contract INTERFACE)
target_link_libraries(
    moguet_test_real_curl_link_contract
    INTERFACE "$<LINK_ONLY:CURL::libcurl>"
)

set(MOGUET_TEST_CATALOG_DIR "${CMAKE_CURRENT_BINARY_DIR}/locale")
set(
    MOGUET_TEST_MISSING_CATALOG_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/tests/missing-locale"
)
set(
    MOGUET_TEST_JA_CATALOG
    "${MOGUET_JA_CATALOG}"
)
set(
    MOGUET_TEST_ZZ_CATALOG
    "${MOGUET_TEST_CATALOG_DIR}/zz/LC_MESSAGES/moguet.mo"
)
set(
    MOGUET_TEST_BROKEN_CATALOG
    "${MOGUET_TEST_CATALOG_DIR}/broken/LC_MESSAGES/moguet.mo"
)

moguet_add_catalog(
    "${MOGUET_TEST_ZZ_CATALOG}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/localization/zz.po"
    TRUE
)
moguet_add_catalog(
    "${MOGUET_TEST_BROKEN_CATALOG}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/localization/invalid-format.po"
    FALSE
)
add_custom_target(
    moguet_test_catalogs ALL
    DEPENDS
        "${MOGUET_TEST_ZZ_CATALOG}"
        "${MOGUET_TEST_BROKEN_CATALOG}"
)
add_dependencies(moguet_test_catalogs moguet_catalogs)

function(_moguet_normalize_source_path output_variable input_path)
    if(input_path MATCHES "^\\$<")
        message(
            FATAL_ERROR
            "Generator expressions are not allowed in a test source list: "
            "${input_path}"
        )
    endif()

    get_filename_component(
        _moguet_absolute_path
        "${input_path}"
        ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    if(NOT EXISTS "${_moguet_absolute_path}")
        message(FATAL_ERROR "C++ test source does not exist: ${input_path}")
    endif()
    if(IS_DIRECTORY "${_moguet_absolute_path}")
        message(FATAL_ERROR "C++ test source is a directory: ${input_path}")
    endif()

    file(
        RELATIVE_PATH
        _moguet_relative_path
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${_moguet_absolute_path}"
    )
    string(REPLACE "\\" "/" _moguet_relative_path "${_moguet_relative_path}")
    if(
        _moguet_relative_path STREQUAL ".."
        OR _moguet_relative_path MATCHES "^\\.\\./"
    )
        message(
            FATAL_ERROR
            "C++ test source is outside the repository: ${input_path}"
        )
    endif()

    set("${output_variable}" "${_moguet_relative_path}" PARENT_SCOPE)
endfunction()

function(_moguet_normalize_source_list output_variable)
    set(_moguet_normalized_sources "")
    foreach(_moguet_source IN LISTS ARGN)
        _moguet_normalize_source_path(
            _moguet_normalized_source
            "${_moguet_source}"
        )
        list(APPEND _moguet_normalized_sources "${_moguet_normalized_source}")
    endforeach()
    set("${output_variable}" ${_moguet_normalized_sources} PARENT_SCOPE)
endfunction()

function(_moguet_assert_unique_list label)
    set(_moguet_items ${ARGN})
    set(_moguet_unique_items ${_moguet_items})
    list(REMOVE_DUPLICATES _moguet_unique_items)
    list(LENGTH _moguet_items _moguet_item_count)
    list(LENGTH _moguet_unique_items _moguet_unique_item_count)
    if(NOT _moguet_item_count EQUAL _moguet_unique_item_count)
        message(FATAL_ERROR "${label} contains duplicate entries")
    endif()
endfunction()

function(
    _moguet_canonicalize_descriptor_value
    output_variable
    input_value
)
    set(_moguet_canonical_value "${input_value}")
    string(
        REPLACE
        "${CMAKE_CURRENT_BINARY_DIR}"
        "<BINARY_DIR>"
        _moguet_canonical_value
        "${_moguet_canonical_value}"
    )
    string(
        REPLACE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "<SOURCE_DIR>"
        _moguet_canonical_value
        "${_moguet_canonical_value}"
    )
    set(
        "${output_variable}"
        "${_moguet_canonical_value}"
        PARENT_SCOPE
    )
endfunction()

# Encode both field and value lengths so a changed list boundary, separator,
# path, or option cannot produce the same descriptor through concatenation.
function(_moguet_append_descriptor_list descriptor_variable field_name)
    set(_moguet_descriptor "${${descriptor_variable}}")
    set(_moguet_values ${ARGN})
    string(LENGTH "${field_name}" _moguet_field_name_length)
    list(LENGTH _moguet_values _moguet_value_count)
    string(
        APPEND
        _moguet_descriptor
        "F${_moguet_field_name_length}:${field_name}"
        "C${_moguet_value_count}:"
    )
    foreach(_moguet_value IN LISTS _moguet_values)
        _moguet_canonicalize_descriptor_value(
            _moguet_canonical_value
            "${_moguet_value}"
        )
        string(LENGTH "${_moguet_canonical_value}" _moguet_value_length)
        string(
            APPEND
            _moguet_descriptor
            "V${_moguet_value_length}:${_moguet_canonical_value}"
        )
    endforeach()
    set(
        "${descriptor_variable}"
        "${_moguet_descriptor}"
        PARENT_SCOPE
    )
endfunction()

function(_moguet_collect_support_sources output_variable)
    set(_moguet_support_sources "")
    foreach(_moguet_source IN LISTS ARGN)
        if(
            _moguet_source STREQUAL "tests/commands_inspect_aur_stub.cpp"
            OR _moguet_source MATCHES "^tests/stubs/.+\\.cpp$"
        )
            list(APPEND _moguet_support_sources "${_moguet_source}")
        endif()
    endforeach()
    set("${output_variable}" ${_moguet_support_sources} PARENT_SCOPE)
endfunction()

function(_moguet_configure_test_compile_owner target_name)
    set(_moguet_options ALPM_COMPILE CURL)
    set(
        _moguet_multi_value_arguments
        DEFINITIONS
        INCLUDE_DIRECTORIES
        COMPILE_OPTIONS
    )
    cmake_parse_arguments(
        _moguet_owner
        "${_moguet_options}"
        ""
        "${_moguet_multi_value_arguments}"
        ${ARGN}
    )
    if(_moguet_owner_UNPARSED_ARGUMENTS)
        message(
            FATAL_ERROR
            "Unexpected compile-owner arguments for ${target_name}: "
            "${_moguet_owner_UNPARSED_ARGUMENTS}"
        )
    endif()

    set_target_properties(
        "${target_name}"
        PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
    )
    target_link_libraries(
        "${target_name}"
        PRIVATE
            moguet_external_cppflags
            moguet_build_contract
            nlohmann_json::nlohmann_json
            moguet_toml_headers
    )
    if(_moguet_owner_ALPM_COMPILE)
        target_link_libraries(
            "${target_name}"
            PRIVATE moguet_alpm_compile_contract
        )
    endif()
    if(_moguet_owner_CURL)
        target_link_libraries("${target_name}" PRIVATE CURL::libcurl)
    endif()
    if(_moguet_owner_DEFINITIONS)
        set(
            _moguet_definition_contract
            "${target_name}__moguet_test_definitions"
        )
        if(TARGET "${_moguet_definition_contract}")
            message(
                FATAL_ERROR
                "Duplicate test definition contract: "
                "${_moguet_definition_contract}"
            )
        endif()
        set(_moguet_definition_options "")
        foreach(_moguet_definition IN LISTS _moguet_owner_DEFINITIONS)
            string(REGEX REPLACE "^-D" "" _moguet_definition "${_moguet_definition}")
            if(
                _moguet_definition STREQUAL ""
                OR _moguet_definition MATCHES "^-U"
            )
                message(
                    FATAL_ERROR
                    "Invalid test compile definition for ${target_name}: "
                    "${_moguet_definition}"
                )
            endif()
            list(APPEND _moguet_definition_options "-D${_moguet_definition}")
        endforeach()
        # Legacy Make places target-owned definitions after external CPPFLAGS.
        # Compile definitions are otherwise emitted before compile options by
        # CMake, allowing an external -U to disable a required test seam.  A
        # target-specific trailing usage contract preserves that ordering
        # without changing the global CPPFLAGS bridge.
        add_library("${_moguet_definition_contract}" INTERFACE)
        target_compile_options(
            "${_moguet_definition_contract}"
            INTERFACE ${_moguet_definition_options}
        )
        target_link_libraries(
            "${target_name}"
            PRIVATE "${_moguet_definition_contract}"
        )
    endif()
    if(_moguet_owner_INCLUDE_DIRECTORIES)
        target_include_directories(
            "${target_name}"
            PRIVATE ${_moguet_owner_INCLUDE_DIRECTORIES}
        )
    endif()
    if(_moguet_owner_COMPILE_OPTIONS)
        target_compile_options(
            "${target_name}"
            PRIVATE ${_moguet_owner_COMPILE_OPTIONS}
        )
    endif()
endfunction()

function(moguet_add_cpp_test target_name)
    set(_moguet_options FIREWALL ALPM_COMPILE REAL_ALPM CURL)
    set(
        _moguet_multi_value_arguments
        SOURCES
        OBJECTS
        OBJECT_PRODUCTION_SOURCES
        DEFINITIONS
        INCLUDE_DIRECTORIES
        COMPILE_OPTIONS
        LINK_OPTIONS
        FORBIDDEN_SOURCES
    )
    cmake_parse_arguments(
        _moguet_test
        "${_moguet_options}"
        ""
        "${_moguet_multi_value_arguments}"
        ${ARGN}
    )
    if(_moguet_test_UNPARSED_ARGUMENTS)
        message(
            FATAL_ERROR
            "Unexpected C++ test arguments for ${target_name}: "
            "${_moguet_test_UNPARSED_ARGUMENTS}"
        )
    endif()
    if(TARGET "${target_name}")
        message(FATAL_ERROR "Duplicate C++ test target: ${target_name}")
    endif()
    if(NOT _moguet_test_SOURCES AND NOT _moguet_test_OBJECTS)
        message(FATAL_ERROR "C++ test target has no sources: ${target_name}")
    endif()

    _moguet_normalize_source_list(
        _moguet_direct_sources
        ${_moguet_test_SOURCES}
    )
    _moguet_normalize_source_list(
        _moguet_declared_object_production_sources
        ${_moguet_test_OBJECT_PRODUCTION_SOURCES}
    )
    _moguet_normalize_source_list(
        _moguet_forbidden_sources
        ${_moguet_test_FORBIDDEN_SOURCES}
    )
    _moguet_assert_unique_list(
        "C++ test ${target_name} direct source list"
        ${_moguet_direct_sources}
    )
    _moguet_assert_unique_list(
        "C++ test ${target_name} declared object production source list"
        ${_moguet_declared_object_production_sources}
    )
    _moguet_assert_unique_list(
        "C++ test ${target_name} forbidden source list"
        ${_moguet_forbidden_sources}
    )

    foreach(
        _moguet_object_production_source
        IN LISTS _moguet_declared_object_production_sources
    )
        list(
            FIND
            MOGUET_PRODUCTION_SOURCES
            "${_moguet_object_production_source}"
            _moguet_production_source_index
        )
        if(_moguet_production_source_index EQUAL -1)
            message(
                FATAL_ERROR
                "C++ test ${target_name} declares a non-production object "
                "source as production: ${_moguet_object_production_source}"
            )
        endif()
    endforeach()

    set(_moguet_object_expressions "")
    set(_moguet_object_sources "")
    set(_moguet_object_target_names "")
    set(_moguet_object_descriptor "")
    set(_moguet_object_production_sources "")
    set(_moguet_uses_production_aggregate FALSE)
    set(_moguet_object_index 0)
    foreach(_moguet_object IN LISTS _moguet_test_OBJECTS)
        if(_moguet_object MATCHES "^\\$<TARGET_OBJECTS:([^>]+)>$")
            set(_moguet_object_target "${CMAKE_MATCH_1}")
            set(_moguet_object_expression "${_moguet_object}")
        elseif(TARGET "${_moguet_object}")
            set(_moguet_object_target "${_moguet_object}")
            set(
                _moguet_object_expression
                "$<TARGET_OBJECTS:${_moguet_object_target}>"
            )
        else()
            message(
                FATAL_ERROR
                "C++ test ${target_name} references an invalid object target: "
                "${_moguet_object}"
            )
        endif()
        if(NOT TARGET "${_moguet_object_target}")
            message(
                FATAL_ERROR
                "C++ test ${target_name} references an unknown object target: "
                "${_moguet_object_target}"
            )
        endif()
        get_target_property(
            _moguet_object_type
            "${_moguet_object_target}"
            TYPE
        )
        if(NOT _moguet_object_type STREQUAL "OBJECT_LIBRARY")
            message(
                FATAL_ERROR
                "C++ test ${target_name} object input is not an OBJECT "
                "library: ${_moguet_object_target}"
            )
        endif()
        list(
            FIND
            _moguet_object_expressions
            "${_moguet_object_expression}"
            _moguet_object_expression_index
        )
        if(NOT _moguet_object_expression_index EQUAL -1)
            message(
                FATAL_ERROR
                "C++ test ${target_name} references an object target twice: "
                "${_moguet_object_target}"
            )
        endif()

        get_target_property(
            _moguet_current_object_sources
            "${_moguet_object_target}"
            SOURCES
        )
        if(
            NOT _moguet_current_object_sources
            OR _moguet_current_object_sources STREQUAL
                "_moguet_current_object_sources-NOTFOUND"
        )
            message(
                FATAL_ERROR
                "C++ test ${target_name} object target has no source "
                "authority: ${_moguet_object_target}"
            )
        endif()
        _moguet_normalize_source_list(
            _moguet_normalized_object_sources
            ${_moguet_current_object_sources}
        )
        _moguet_assert_unique_list(
            "C++ test ${target_name} object source list for ${_moguet_object_target}"
            ${_moguet_normalized_object_sources}
        )
        list(
            APPEND
            _moguet_object_target_names
            "${_moguet_object_target}"
        )
        _moguet_append_descriptor_list(
            _moguet_object_descriptor
            "object.${_moguet_object_index}.target"
            "${_moguet_object_target}"
        )
        _moguet_append_descriptor_list(
            _moguet_object_descriptor
            "object.${_moguet_object_index}.sources"
            ${_moguet_normalized_object_sources}
        )
        math(EXPR _moguet_object_index "${_moguet_object_index} + 1")
        list(
            APPEND
            _moguet_object_sources
            ${_moguet_normalized_object_sources}
        )
        foreach(
            _moguet_current_object_source
            IN LISTS _moguet_normalized_object_sources
        )
            list(
                FIND
                MOGUET_PRODUCTION_SOURCES
                "${_moguet_current_object_source}"
                _moguet_current_production_source_index
            )
            if(NOT _moguet_current_production_source_index EQUAL -1)
                list(
                    APPEND
                    _moguet_object_production_sources
                    "${_moguet_current_object_source}"
                )
            endif()
        endforeach()

        if(_moguet_object_target STREQUAL "moguet_production_objects")
            set(_moguet_uses_production_aggregate TRUE)
        endif()

        set(
            _moguet_existing_production_object
            FALSE
        )
        if(
            _moguet_object_target STREQUAL "moguet_production_objects"
            OR _moguet_object_target STREQUAL
                "moguet_unified_plan_projection_object"
            OR _moguet_object_target STREQUAL
                "moguet_unified_plan_renderer_object"
        )
            set(_moguet_existing_production_object TRUE)
        endif()
        if(NOT _moguet_existing_production_object)
            list(
                FIND
                MOGUET_CPP_TEST_OBJECT_TARGETS
                "${_moguet_object_target}"
                _moguet_existing_object_target_index
            )
            if(NOT _moguet_existing_object_target_index EQUAL -1)
                message(
                    FATAL_ERROR
                    "Test object target is shared across C++ test profiles: "
                    "${_moguet_object_target}"
                )
            endif()
            set(_moguet_owner_options "")
            if(_moguet_test_ALPM_COMPILE)
                list(APPEND _moguet_owner_options ALPM_COMPILE)
            endif()
            if(_moguet_test_CURL)
                list(APPEND _moguet_owner_options CURL)
            endif()
            _moguet_configure_test_compile_owner(
                "${_moguet_object_target}"
                ${_moguet_owner_options}
                DEFINITIONS ${_moguet_test_DEFINITIONS}
                INCLUDE_DIRECTORIES ${_moguet_test_INCLUDE_DIRECTORIES}
                COMPILE_OPTIONS ${_moguet_test_COMPILE_OPTIONS}
            )
            list(
                APPEND
                MOGUET_CPP_TEST_OBJECT_TARGETS
                "${_moguet_object_target}"
            )
        endif()
        list(APPEND _moguet_object_expressions "${_moguet_object_expression}")
    endforeach()

    _moguet_assert_unique_list(
        "C++ test ${target_name} combined object source list"
        ${_moguet_object_sources}
    )
    set(_moguet_logical_sources ${_moguet_direct_sources})
    list(APPEND _moguet_logical_sources ${_moguet_object_sources})
    _moguet_assert_unique_list(
        "C++ test ${target_name} complete logical source list"
        ${_moguet_logical_sources}
    )

    set(_moguet_unique_object_production_sources ${_moguet_object_production_sources})
    list(REMOVE_DUPLICATES _moguet_unique_object_production_sources)
    set(_moguet_expected_object_production_sources ${_moguet_declared_object_production_sources})
    list(SORT _moguet_unique_object_production_sources)
    list(SORT _moguet_expected_object_production_sources)
    if(
        NOT "${_moguet_unique_object_production_sources}" STREQUAL
            "${_moguet_expected_object_production_sources}"
    )
        message(
            FATAL_ERROR
            "C++ test ${target_name} object production-source declaration "
            "does not match its OBJECT inputs"
        )
    endif()

    if(
        _moguet_uses_production_aggregate
        AND NOT target_name STREQUAL "moguet-root-execution-identity-test"
    )
        message(
            FATAL_ERROR
            "Only moguet-root-execution-identity-test may consume the broad "
            "moguet_production_objects aggregate; ${target_name} must use an "
            "exact source closure"
        )
    endif()
    foreach(_moguet_forbidden_source IN LISTS _moguet_forbidden_sources)
        list(
            FIND
            _moguet_logical_sources
            "${_moguet_forbidden_source}"
            _moguet_forbidden_source_index
        )
        if(NOT _moguet_forbidden_source_index EQUAL -1)
            message(
                FATAL_ERROR
                "C++ test ${target_name} includes forbidden source: "
                "${_moguet_forbidden_source}"
            )
        endif()
    endforeach()

    list(
        FIND
        _moguet_logical_sources
        "tests/stubs/package-metadata/alpm_stub.cpp"
        _moguet_alpm_stub_index
    )
    if(NOT _moguet_alpm_stub_index EQUAL -1)
        if(_moguet_test_REAL_ALPM)
            message(
                FATAL_ERROR
                "C++ test ${target_name} mixes the ALPM stub with real libalpm"
            )
        endif()
        if(NOT _moguet_test_ALPM_COMPILE)
            message(
                FATAL_ERROR
                "C++ test ${target_name} uses the ALPM stub without the "
                "libalpm compile contract"
            )
        endif()
    endif()

    if(_moguet_test_FIREWALL)
        set(_moguet_firewall_descriptor "")
        _moguet_append_descriptor_list(
            _moguet_firewall_descriptor
            schema
            moguet-cpp-test-firewall-v1
        )
        _moguet_append_descriptor_list(
            _moguet_firewall_descriptor
            target
            "${target_name}"
        )
        _moguet_append_descriptor_list(
            _moguet_firewall_descriptor
            direct_sources
            ${_moguet_direct_sources}
        )
        _moguet_append_descriptor_list(
            _moguet_firewall_descriptor
            object_targets
            ${_moguet_object_target_names}
        )
        string(
            APPEND
            _moguet_firewall_descriptor
            "${_moguet_object_descriptor}"
        )
        _moguet_append_descriptor_list(
            _moguet_firewall_descriptor
            object_production_sources
            ${_moguet_declared_object_production_sources}
        )
        _moguet_append_descriptor_list(
            _moguet_firewall_descriptor
            logical_sources
            ${_moguet_logical_sources}
        )
        _moguet_append_descriptor_list(
            _moguet_firewall_descriptor
            alpm_compile
            "${_moguet_test_ALPM_COMPILE}"
        )
        _moguet_append_descriptor_list(
            _moguet_firewall_descriptor
            real_alpm
            "${_moguet_test_REAL_ALPM}"
        )
        _moguet_append_descriptor_list(
            _moguet_firewall_descriptor
            curl
            "${_moguet_test_CURL}"
        )
        _moguet_append_descriptor_list(
            _moguet_firewall_descriptor
            definitions
            ${_moguet_test_DEFINITIONS}
        )
        _moguet_append_descriptor_list(
            _moguet_firewall_descriptor
            include_directories
            ${_moguet_test_INCLUDE_DIRECTORIES}
        )
        _moguet_append_descriptor_list(
            _moguet_firewall_descriptor
            compile_options
            ${_moguet_test_COMPILE_OPTIONS}
        )
        _moguet_append_descriptor_list(
            _moguet_firewall_descriptor
            link_options
            ${_moguet_test_LINK_OPTIONS}
        )
        _moguet_append_descriptor_list(
            _moguet_firewall_descriptor
            forbidden_sources
            ${_moguet_forbidden_sources}
        )
        string(
            SHA256
            _moguet_firewall_descriptor_sha256
            "${_moguet_firewall_descriptor}"
        )
        list(
            APPEND
            MOGUET_CPP_TEST_FIREWALL_DESCRIPTORS
            "${target_name}=${_moguet_firewall_descriptor_sha256}"
        )
    endif()

    add_executable(
        "${target_name}"
        ${_moguet_direct_sources}
        ${_moguet_object_expressions}
    )
    set_target_properties(
        "${target_name}"
        PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    set(_moguet_owner_options "")
    if(_moguet_test_ALPM_COMPILE)
        list(APPEND _moguet_owner_options ALPM_COMPILE)
    endif()
    if(_moguet_test_CURL AND NOT _moguet_uses_production_aggregate)
        list(APPEND _moguet_owner_options CURL)
    endif()
    _moguet_configure_test_compile_owner(
        "${target_name}"
        ${_moguet_owner_options}
        DEFINITIONS ${_moguet_test_DEFINITIONS}
        INCLUDE_DIRECTORIES ${_moguet_test_INCLUDE_DIRECTORIES}
        COMPILE_OPTIONS ${_moguet_test_COMPILE_OPTIONS}
    )
    if(_moguet_test_REAL_ALPM)
        target_link_libraries(
            "${target_name}"
            PRIVATE moguet_test_real_alpm_link_contract
        )
    endif()
    if(_moguet_test_CURL AND _moguet_uses_production_aggregate)
        target_link_libraries(
            "${target_name}"
            PRIVATE moguet_test_real_curl_link_contract
        )
    endif()
    if(_moguet_test_LINK_OPTIONS)
        target_link_options(
            "${target_name}"
            PRIVATE ${_moguet_test_LINK_OPTIONS}
        )
    endif()

    _moguet_collect_support_sources(
        _moguet_current_support_sources
        ${_moguet_logical_sources}
    )
    list(APPEND MOGUET_CPP_TEST_TARGETS "${target_name}")
    list(
        APPEND
        MOGUET_CPP_TEST_SUPPORT_SOURCES
        ${_moguet_current_support_sources}
    )
    list(REMOVE_DUPLICATES MOGUET_CPP_TEST_SUPPORT_SOURCES)
    if(_moguet_test_FIREWALL)
        list(APPEND MOGUET_CPP_TEST_FIREWALL_TARGETS "${target_name}")
        set_property(
            TARGET "${target_name}"
            PROPERTY MOGUET_FIREWALL_DESCRIPTOR_SHA256
                "${_moguet_firewall_descriptor_sha256}"
        )
    endif()

    set(MOGUET_CPP_TEST_TARGETS ${MOGUET_CPP_TEST_TARGETS} PARENT_SCOPE)
    set(
        MOGUET_CPP_TEST_SUPPORT_SOURCES
        ${MOGUET_CPP_TEST_SUPPORT_SOURCES}
        PARENT_SCOPE
    )
    set(
        MOGUET_CPP_TEST_FIREWALL_TARGETS
        ${MOGUET_CPP_TEST_FIREWALL_TARGETS}
        PARENT_SCOPE
    )
    set(
        MOGUET_CPP_TEST_FIREWALL_DESCRIPTORS
        ${MOGUET_CPP_TEST_FIREWALL_DESCRIPTORS}
        PARENT_SCOPE
    )
    set(
        MOGUET_CPP_TEST_OBJECT_TARGETS
        ${MOGUET_CPP_TEST_OBJECT_TARGETS}
        PARENT_SCOPE
    )
endfunction()

function(moguet_add_ctest)
    set(_moguet_one_value_arguments NAME)
    set(_moguet_multi_value_arguments TARGETS COMMAND ENVIRONMENT)
    cmake_parse_arguments(
        _moguet_ctest
        ""
        "${_moguet_one_value_arguments}"
        "${_moguet_multi_value_arguments}"
        ${ARGN}
    )
    if(_moguet_ctest_UNPARSED_ARGUMENTS)
        message(
            FATAL_ERROR
            "Unexpected CTest arguments: ${_moguet_ctest_UNPARSED_ARGUMENTS}"
        )
    endif()
    if(NOT _moguet_ctest_NAME)
        message(FATAL_ERROR "moguet_add_ctest requires NAME")
    endif()
    if(NOT _moguet_ctest_COMMAND)
        message(
            FATAL_ERROR
            "CTest ${_moguet_ctest_NAME} requires a COMMAND"
        )
    endif()
    _moguet_assert_unique_list(
        "CTest ${_moguet_ctest_NAME} target usage list"
        ${_moguet_ctest_TARGETS}
    )
    list(
        FIND
        MOGUET_CTEST_NAMES
        "${_moguet_ctest_NAME}"
        _moguet_existing_ctest_name_index
    )
    if(NOT _moguet_existing_ctest_name_index EQUAL -1)
        message(FATAL_ERROR "Duplicate CTest name: ${_moguet_ctest_NAME}")
    endif()

    foreach(_moguet_runtime_target IN LISTS _moguet_ctest_TARGETS)
        if(NOT TARGET "${_moguet_runtime_target}")
            message(
                FATAL_ERROR
                "CTest ${_moguet_ctest_NAME} references an unknown target: "
                "${_moguet_runtime_target}"
            )
        endif()
        list(
            FIND
            MOGUET_CPP_TEST_TARGETS
            "${_moguet_runtime_target}"
            _moguet_cpp_test_target_index
        )
        if(NOT _moguet_cpp_test_target_index EQUAL -1)
            list(
                APPEND
                MOGUET_CTEST_RUNTIME_TARGETS
                "${_moguet_runtime_target}"
            )
        endif()
        string(
            FIND
            "${_moguet_ctest_COMMAND}"
            "$<TARGET_FILE:${_moguet_runtime_target}>"
            _moguet_target_file_usage_index
        )
        string(
            FIND
            "${_moguet_ctest_COMMAND}"
            "$<TARGET_OBJECTS:${_moguet_runtime_target}>"
            _moguet_target_object_usage_index
        )
        if(
            _moguet_target_file_usage_index EQUAL -1
            AND _moguet_target_object_usage_index EQUAL -1
        )
            message(
                FATAL_ERROR
                "CTest ${_moguet_ctest_NAME} accounts target "
                "${_moguet_runtime_target} without using its artifact"
            )
        endif()
    endforeach()
    list(REMOVE_DUPLICATES MOGUET_CTEST_RUNTIME_TARGETS)

    add_test(
        NAME "${_moguet_ctest_NAME}"
        COMMAND ${_moguet_ctest_COMMAND}
    )
    string(SHA256 _moguet_ctest_key "${_moguet_ctest_NAME}")
    set_property(
        GLOBAL
        PROPERTY "MOGUET_CTEST_TARGETS_${_moguet_ctest_key}"
            "${_moguet_ctest_TARGETS}"
    )
    set_tests_properties(
        "${_moguet_ctest_NAME}"
        PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    if(_moguet_ctest_ENVIRONMENT)
        set_tests_properties(
            "${_moguet_ctest_NAME}"
            PROPERTIES ENVIRONMENT "${_moguet_ctest_ENVIRONMENT}"
        )
    endif()

    list(APPEND MOGUET_CTEST_NAMES "${_moguet_ctest_NAME}")
    set(MOGUET_CTEST_NAMES ${MOGUET_CTEST_NAMES} PARENT_SCOPE)
    set(
        MOGUET_CTEST_RUNTIME_TARGETS
        ${MOGUET_CTEST_RUNTIME_TARGETS}
        PARENT_SCOPE
    )
endfunction()

function(moguet_add_focused_ctest_alias alias_name)
    set(_moguet_multi_value_arguments TESTS TARGETS)
    cmake_parse_arguments(
        _moguet_focus
        ""
        ""
        "${_moguet_multi_value_arguments}"
        ${ARGN}
    )
    if(_moguet_focus_UNPARSED_ARGUMENTS)
        message(
            FATAL_ERROR
            "Unexpected focused CTest alias arguments for ${alias_name}: "
            "${_moguet_focus_UNPARSED_ARGUMENTS}"
        )
    endif()
    if(NOT alias_name MATCHES "^test-[a-z0-9-]+$")
        message(FATAL_ERROR "Invalid focused Make alias: ${alias_name}")
    endif()
    if(NOT _moguet_focus_TESTS)
        message(FATAL_ERROR "Focused Make alias has no CTest: ${alias_name}")
    endif()

    set(_moguet_focus_target "moguet-focus-${alias_name}")
    if(TARGET "${_moguet_focus_target}")
        message(FATAL_ERROR "Duplicate focused Make alias: ${alias_name}")
    endif()

    _moguet_assert_unique_list(
        "Focused Make alias ${alias_name} CTest list"
        ${_moguet_focus_TESTS}
    )
    set(_moguet_focus_build_targets ${_moguet_focus_TARGETS})
    set(_moguet_focus_regex_items "")
    foreach(_moguet_focus_test IN LISTS _moguet_focus_TESTS)
        list(
            FIND
            MOGUET_CTEST_NAMES
            "${_moguet_focus_test}"
            _moguet_focus_test_index
        )
        if(_moguet_focus_test_index EQUAL -1)
            message(
                FATAL_ERROR
                "Focused Make alias ${alias_name} references an unknown "
                "CTest: ${_moguet_focus_test}"
            )
        endif()

        string(SHA256 _moguet_focus_test_key "${_moguet_focus_test}")
        get_property(
            _moguet_focus_test_targets
            GLOBAL
            PROPERTY "MOGUET_CTEST_TARGETS_${_moguet_focus_test_key}"
        )
        list(
            APPEND
            _moguet_focus_build_targets
            ${_moguet_focus_test_targets}
        )
        string(
            REPLACE
            "."
            "\\."
            _moguet_focus_test_regex
            "${_moguet_focus_test}"
        )
        list(APPEND _moguet_focus_regex_items "${_moguet_focus_test_regex}")
    endforeach()
    list(REMOVE_DUPLICATES _moguet_focus_build_targets)
    foreach(_moguet_focus_build_target IN LISTS _moguet_focus_build_targets)
        if(NOT TARGET "${_moguet_focus_build_target}")
            message(
                FATAL_ERROR
                "Focused Make alias ${alias_name} references an unknown "
                "build target: ${_moguet_focus_build_target}"
            )
        endif()
    endforeach()
    list(JOIN _moguet_focus_regex_items "|" _moguet_focus_regex)

    add_custom_target(
        "${_moguet_focus_target}"
        COMMAND
            "${CMAKE_CTEST_COMMAND}"
            --output-on-failure
            --tests-regex "^(${_moguet_focus_regex})$"
        DEPENDS ${_moguet_focus_build_targets}
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        USES_TERMINAL
        VERBATIM
    )
endfunction()

function(_moguet_assert_exact_inventory label expected_variable actual_variable)
    if(NOT DEFINED ${expected_variable})
        message(
            FATAL_ERROR
            "Missing expected inventory ${expected_variable} for ${label}"
        )
    endif()

    set(_moguet_expected ${${expected_variable}})
    set(_moguet_actual ${${actual_variable}})
    _moguet_assert_unique_list("Expected ${label}" ${_moguet_expected})
    _moguet_assert_unique_list("Accounted ${label}" ${_moguet_actual})

    set(_moguet_missing "")
    foreach(_moguet_item IN LISTS _moguet_expected)
        list(FIND _moguet_actual "${_moguet_item}" _moguet_item_index)
        if(_moguet_item_index EQUAL -1)
            list(APPEND _moguet_missing "${_moguet_item}")
        endif()
    endforeach()
    set(_moguet_unexpected "")
    foreach(_moguet_item IN LISTS _moguet_actual)
        list(FIND _moguet_expected "${_moguet_item}" _moguet_item_index)
        if(_moguet_item_index EQUAL -1)
            list(APPEND _moguet_unexpected "${_moguet_item}")
        endif()
    endforeach()
    if(_moguet_missing OR _moguet_unexpected)
        list(SORT _moguet_missing)
        list(SORT _moguet_unexpected)
        string(REPLACE ";" ", " _moguet_missing_text "${_moguet_missing}")
        string(
            REPLACE
            ";"
            ", "
            _moguet_unexpected_text
            "${_moguet_unexpected}"
        )
        message(
            FATAL_ERROR
            "${label} inventory mismatch; missing=[${_moguet_missing_text}], "
            "unexpected=[${_moguet_unexpected_text}]"
        )
    endif()
endfunction()

function(
    _moguet_validate_firewall_descriptor_ledger
    label
    ledger_variable
    output_target_variable
)
    set(_moguet_descriptor_entries ${${ledger_variable}})
    _moguet_assert_unique_list(
        "${label} descriptor ledger"
        ${_moguet_descriptor_entries}
    )
    set(_moguet_descriptor_targets "")
    foreach(_moguet_descriptor_entry IN LISTS _moguet_descriptor_entries)
        if(
            NOT _moguet_descriptor_entry MATCHES
                "^([^=]+)=([0-9a-f]+)$"
        )
            message(
                FATAL_ERROR
                "Invalid ${label} firewall descriptor entry: "
                "${_moguet_descriptor_entry}"
            )
        endif()
        set(_moguet_descriptor_target "${CMAKE_MATCH_1}")
        set(_moguet_descriptor_sha256 "${CMAKE_MATCH_2}")
        string(
            LENGTH
            "${_moguet_descriptor_sha256}"
            _moguet_descriptor_sha256_length
        )
        if(NOT _moguet_descriptor_sha256_length EQUAL 64)
            message(
                FATAL_ERROR
                "Invalid ${label} firewall SHA256 for "
                "${_moguet_descriptor_target}: "
                "${_moguet_descriptor_sha256}"
            )
        endif()
        list(
            APPEND
            _moguet_descriptor_targets
            "${_moguet_descriptor_target}"
        )
    endforeach()
    _moguet_assert_unique_list(
        "${label} firewall descriptor target list"
        ${_moguet_descriptor_targets}
    )
    set(
        "${output_target_variable}"
        ${_moguet_descriptor_targets}
        PARENT_SCOPE
    )
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/MoguetTestTargets.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/MoguetTestRegistrations.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/MoguetFocusedTests.cmake")

foreach(
    _moguet_expected_inventory
    IN ITEMS
        MOGUET_EXPECTED_CPP_TEST_TARGETS
        MOGUET_EXPECTED_CPP_TEST_SUPPORT_SOURCES
        MOGUET_EXPECTED_CPP_TEST_FIREWALL_TARGETS
        MOGUET_EXPECTED_CPP_TEST_FIREWALL_DESCRIPTORS
)
    if(NOT DEFINED ${_moguet_expected_inventory})
        message(
            FATAL_ERROR
            "Test manifest did not define ${_moguet_expected_inventory}"
        )
    endif()
endforeach()

list(LENGTH MOGUET_EXPECTED_CPP_TEST_TARGETS _moguet_expected_target_count)
list(
    LENGTH
    MOGUET_EXPECTED_CPP_TEST_SUPPORT_SOURCES
    _moguet_expected_support_count
)
list(
    LENGTH
    MOGUET_EXPECTED_CPP_TEST_FIREWALL_TARGETS
    _moguet_expected_firewall_count
)
list(
    LENGTH
    MOGUET_EXPECTED_CPP_TEST_FIREWALL_DESCRIPTORS
    _moguet_expected_firewall_descriptor_count
)
if(NOT _moguet_expected_target_count EQUAL 115)
    message(
        FATAL_ERROR
        "Expected C++ test target inventory must contain 115 entries, got "
        "${_moguet_expected_target_count}"
    )
endif()
if(NOT _moguet_expected_support_count EQUAL 32)
    message(
        FATAL_ERROR
        "Expected test support/stub inventory must contain 32 entries, got "
        "${_moguet_expected_support_count}"
    )
endif()
if(NOT _moguet_expected_firewall_count EQUAL 50)
    message(
        FATAL_ERROR
        "Expected link firewall inventory must contain 50 entries, got "
        "${_moguet_expected_firewall_count}"
    )
endif()
if(NOT _moguet_expected_firewall_descriptor_count EQUAL 50)
    message(
        FATAL_ERROR
        "Expected link firewall descriptor inventory must contain 50 "
        "entries, got ${_moguet_expected_firewall_descriptor_count}"
    )
endif()

_moguet_validate_firewall_descriptor_ledger(
    Expected
    MOGUET_EXPECTED_CPP_TEST_FIREWALL_DESCRIPTORS
    _moguet_expected_firewall_descriptor_targets
)
_moguet_validate_firewall_descriptor_ledger(
    Accounted
    MOGUET_CPP_TEST_FIREWALL_DESCRIPTORS
    _moguet_actual_firewall_descriptor_targets
)

_moguet_assert_exact_inventory(
    "C++ test target"
    MOGUET_EXPECTED_CPP_TEST_TARGETS
    MOGUET_CPP_TEST_TARGETS
)
_moguet_assert_exact_inventory(
    "test support/stub source"
    MOGUET_EXPECTED_CPP_TEST_SUPPORT_SOURCES
    MOGUET_CPP_TEST_SUPPORT_SOURCES
)
_moguet_assert_exact_inventory(
    "link firewall target"
    MOGUET_EXPECTED_CPP_TEST_FIREWALL_TARGETS
    MOGUET_CPP_TEST_FIREWALL_TARGETS
)
_moguet_assert_exact_inventory(
    "expected link firewall descriptor target"
    MOGUET_EXPECTED_CPP_TEST_FIREWALL_TARGETS
    _moguet_expected_firewall_descriptor_targets
)
_moguet_assert_exact_inventory(
    "accounted link firewall descriptor target"
    MOGUET_EXPECTED_CPP_TEST_FIREWALL_TARGETS
    _moguet_actual_firewall_descriptor_targets
)
_moguet_assert_exact_inventory(
    "link firewall descriptor"
    MOGUET_EXPECTED_CPP_TEST_FIREWALL_DESCRIPTORS
    MOGUET_CPP_TEST_FIREWALL_DESCRIPTORS
)
_moguet_assert_exact_inventory(
    "CTest runtime target"
    MOGUET_EXPECTED_CPP_TEST_TARGETS
    MOGUET_CTEST_RUNTIME_TARGETS
)

list(LENGTH MOGUET_CPP_TEST_TARGETS _moguet_test_target_count)
list(LENGTH MOGUET_CPP_TEST_SUPPORT_SOURCES _moguet_test_support_count)
list(LENGTH MOGUET_CPP_TEST_FIREWALL_TARGETS _moguet_test_firewall_count)
list(
    LENGTH
    MOGUET_CPP_TEST_FIREWALL_DESCRIPTORS
    _moguet_test_firewall_descriptor_count
)
list(LENGTH MOGUET_CTEST_NAMES _moguet_ctest_count)
message(
    STATUS
    "Moguet C++ tests: targets=${_moguet_test_target_count}/115, "
    "support=${_moguet_test_support_count}/32, "
    "firewalls=${_moguet_test_firewall_count}/50, "
    "descriptors=${_moguet_test_firewall_descriptor_count}/50, "
    "CTest registrations=${_moguet_ctest_count}"
)
