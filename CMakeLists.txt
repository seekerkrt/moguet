cmake_minimum_required(VERSION 3.18)
include(cmake/MoguetPolicies.cmake NO_POLICY_SCOPE)

# This is an invocation-scoped frontend signal, not persistent provenance.
# Make and PKGBUILD pass ON on every canonical configure; ordinary direct
# CMake callers retain the normal environment/cache precedence.
set(_moguet_sync_external_build_inputs OFF)
if(DEFINED MOGUET_SYNC_EXTERNAL_BUILD_INPUTS)
    if("${MOGUET_SYNC_EXTERNAL_BUILD_INPUTS}" STREQUAL "ON")
        set(_moguet_sync_external_build_inputs ON)
    elseif(NOT "${MOGUET_SYNC_EXTERNAL_BUILD_INPUTS}" STREQUAL "OFF")
        message(
            FATAL_ERROR
            "MOGUET_SYNC_EXTERNAL_BUILD_INPUTS must be ON or OFF"
        )
    endif()
endif()
unset(MOGUET_SYNC_EXTERNAL_BUILD_INPUTS CACHE)
unset(MOGUET_SYNC_EXTERNAL_BUILD_INPUTS)

# Make can distinguish an undefined CXXFLAGS from an explicitly empty value.
# Keep that frontend-only distinction invocation-scoped and let CMake remain
# the sole owner of the actual default compile options.
set(_moguet_frontend_use_default_compile_options OFF)
if(DEFINED MOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS)
    if("${MOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS}" STREQUAL "ON")
        set(_moguet_frontend_use_default_compile_options ON)
    elseif(NOT "${MOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS}" STREQUAL "OFF")
        message(
            FATAL_ERROR
            "MOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS must be ON or OFF"
        )
    endif()
endif()
unset(MOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS CACHE)
unset(MOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS)
if(
    _moguet_frontend_use_default_compile_options
    AND NOT _moguet_sync_external_build_inputs
)
    message(
        FATAL_ERROR
        "MOGUET_FRONTEND_USE_DEFAULT_COMPILE_OPTIONS requires "
        "MOGUET_SYNC_EXTERNAL_BUILD_INPUTS=ON"
    )
endif()

if(_moguet_sync_external_build_inputs)
    foreach(
        _moguet_external_build_input
        IN ITEMS CPPFLAGS CXXFLAGS LDFLAGS CCACHE
    )
        if(NOT DEFINED ENV{${_moguet_external_build_input}})
            message(
                FATAL_ERROR
                "Synchronized frontend configure requires an exported "
                "${_moguet_external_build_input} value, including an "
                "explicitly empty value"
            )
        endif()
    endforeach()
endif()

# Plain direct CMake uses the project defaults only when the caller did not
# define CXXFLAGS or CMAKE_CXX_FLAGS. Record that distinction before project()
# initializes CMAKE_CXX_FLAGS, including an explicitly empty value.
set(_moguet_cxxflags_defined_before_project OFF)
if(DEFINED ENV{CXXFLAGS} OR DEFINED CACHE{CMAKE_CXX_FLAGS})
    set(_moguet_cxxflags_defined_before_project ON)
endif()

project(Moguet LANGUAGES CXX)

option(
    MOGUET_DEVELOPER_COMPILE_COMMANDS_LINK
    "Request post-success compile database publication by a supported frontend"
    OFF
)
if(MOGUET_DEVELOPER_COMPILE_COMMANDS_LINK)
    if(NOT CMAKE_EXPORT_COMPILE_COMMANDS)
        message(
            FATAL_ERROR
            "MOGUET_DEVELOPER_COMPILE_COMMANDS_LINK requires "
            "CMAKE_EXPORT_COMPILE_COMMANDS=ON"
        )
    endif()
endif()

# CMake uses CXXFLAGS and LDFLAGS only to initialize a new cache.  The Make
# and PKGBUILD frontends explicitly opt into current-invocation synchronization
# for persistent trees.  Direct CMake calls retain normal cache semantics.
if(_moguet_sync_external_build_inputs)
    set(
        CMAKE_CXX_FLAGS
        "$ENV{CXXFLAGS}"
        CACHE STRING
        "Flags used by the CXX compiler during all build types"
        FORCE
    )
    set(
        CMAKE_EXE_LINKER_FLAGS
        "$ENV{LDFLAGS}"
        CACHE STRING
        "Flags used by the linker during all executable builds"
        FORCE
    )
    separate_arguments(
        _moguet_cxx_compiler_launcher
        UNIX_COMMAND
        "$ENV{CCACHE}"
    )
    set(
        CMAKE_CXX_COMPILER_LAUNCHER
        "${_moguet_cxx_compiler_launcher}"
        CACHE STRING
        "Launcher used for Moguet C++ compilation"
        FORCE
    )
endif()

include(CTest)
include(GNUInstallDirs)

include(cmake/MoguetSources.cmake)

set(
    MOGUET_INSTALL_INTERNAL_EXECUTABLE_DIRECTORY
    "${CMAKE_INSTALL_LIBEXECDIR}/moguet"
    CACHE PATH
    "Installation directory for Moguet internal executables"
)
if(IS_ABSOLUTE "${MOGUET_INSTALL_INTERNAL_EXECUTABLE_DIRECTORY}")
    set(
        MOGUET_FULL_INTERNAL_EXECUTABLE_DIRECTORY
        "${MOGUET_INSTALL_INTERNAL_EXECUTABLE_DIRECTORY}"
    )
else()
    get_filename_component(
        MOGUET_FULL_INTERNAL_EXECUTABLE_DIRECTORY
        "${CMAKE_INSTALL_PREFIX}/${MOGUET_INSTALL_INTERNAL_EXECUTABLE_DIRECTORY}"
        ABSOLUTE
    )
endif()
set(
    MOGUET_ALPM_RECEIPT_HELPER_INSTALL_PATH
    "${MOGUET_FULL_INTERNAL_EXECUTABLE_DIRECTORY}/moguet-alpm-receipt-helper"
)
set(
    MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_INSTALL_PATH
    "${MOGUET_FULL_INTERNAL_EXECUTABLE_DIRECTORY}/moguet-source-artifact-install-helper"
)
if(
    NOT IS_ABSOLUTE "${MOGUET_ALPM_RECEIPT_HELPER_INSTALL_PATH}"
    OR MOGUET_ALPM_RECEIPT_HELPER_INSTALL_PATH MATCHES "//"
    OR MOGUET_ALPM_RECEIPT_HELPER_INSTALL_PATH MATCHES "(^|/)\\.(/|$)"
    OR MOGUET_ALPM_RECEIPT_HELPER_INSTALL_PATH MATCHES "(^|/)\\.\\.(/|$)"
    OR NOT MOGUET_ALPM_RECEIPT_HELPER_INSTALL_PATH MATCHES
        "^/[A-Za-z0-9_./+-]+$"
)
    message(
        FATAL_ERROR
        "MOGUET_INSTALL_INTERNAL_EXECUTABLE_DIRECTORY cannot be represented "
        "as a shell-free ALPM hook Exec path: "
        "${MOGUET_INSTALL_INTERNAL_EXECUTABLE_DIRECTORY}"
    )
endif()
if(
    NOT IS_ABSOLUTE "${MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_INSTALL_PATH}"
    OR MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_INSTALL_PATH MATCHES "//"
    OR MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_INSTALL_PATH MATCHES
        "(^|/)\\.(/|$)"
    OR MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_INSTALL_PATH MATCHES
        "(^|/)\\.\\.(/|$)"
    OR NOT MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_INSTALL_PATH MATCHES
        "^/[A-Za-z0-9_./+-]+$"
)
    message(
        FATAL_ERROR
        "MOGUET_INSTALL_INTERNAL_EXECUTABLE_DIRECTORY cannot be represented "
        "as a shell-free source-artifact hook Exec path: "
        "${MOGUET_INSTALL_INTERNAL_EXECUTABLE_DIRECTORY}"
    )
endif()

find_package(CURL REQUIRED)
# The JSON export resets its policy version before creating its header-only
# target. Adopt CMP0200 there too, without changing any other package policy
# or leaking a default into subsequent find_package calls. Explicit policies
# chosen by the package still take precedence over this default.
if(POLICY CMP0200)
    if(DEFINED CMAKE_POLICY_DEFAULT_CMP0200)
        set(_moguet_saved_cmp0200_default "${CMAKE_POLICY_DEFAULT_CMP0200}")
    endif()
    set(CMAKE_POLICY_DEFAULT_CMP0200 NEW)
endif()
find_package(nlohmann_json CONFIG REQUIRED)
if(POLICY CMP0200)
    if(DEFINED _moguet_saved_cmp0200_default)
        set(CMAKE_POLICY_DEFAULT_CMP0200 "${_moguet_saved_cmp0200_default}")
        unset(_moguet_saved_cmp0200_default)
    else()
        unset(CMAKE_POLICY_DEFAULT_CMP0200)
    endif()
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(ALPM REQUIRED IMPORTED_TARGET libalpm)
find_path(
    TOMLPLUSPLUS_INCLUDE_DIR
    NAMES toml++/toml.hpp
    REQUIRED
)

set(
    MOGUET_LOCALE_DIRECTORY
    "/usr/share/locale"
    CACHE STRING
    "Absolute runtime directory containing Moguet message catalogs"
)
if(NOT IS_ABSOLUTE "${MOGUET_LOCALE_DIRECTORY}")
    message(
        FATAL_ERROR
        "MOGUET_LOCALE_DIRECTORY must be an absolute path: "
        "${MOGUET_LOCALE_DIRECTORY}"
    )
endif()

set(MOGUET_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MOGUET_GENERATED_INCLUDE_DIR}")

# configure_file() keeps VERSION in CMake's configure dependencies. A missing
# VERSION remains an error, while an existing empty file retains the legacy
# "unknown" fallback.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
    "${MOGUET_GENERATED_INCLUDE_DIR}/VERSION"
    COPYONLY
)
file(READ "${MOGUET_GENERATED_INCLUDE_DIR}/VERSION" MOGUET_VERSION)
string(REGEX REPLACE "[ \t\r\n]+" " " MOGUET_VERSION "${MOGUET_VERSION}")
string(STRIP "${MOGUET_VERSION}" MOGUET_VERSION)
if(MOGUET_VERSION STREQUAL "")
    set(MOGUET_VERSION "unknown")
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/localization_config.hpp.in"
    "${MOGUET_GENERATED_INCLUDE_DIR}/localization_config.hpp"
    @ONLY
)

# Production catalogs are runtime artifacts, not test fixtures.  Keep their
# build and install graph available even when package builds disable CTest.
find_program(MOGUET_MSGFMT_EXECUTABLE NAMES msgfmt)
if(NOT MOGUET_MSGFMT_EXECUTABLE)
    message(FATAL_ERROR "msgfmt is required to build the message catalogs")
endif()

function(moguet_add_catalog output_file input_file check_format)
    get_filename_component(_moguet_catalog_directory "${output_file}" DIRECTORY)
    set(_moguet_catalog_temporary_file "${output_file}.tmp")
    if(check_format)
        set(
            _moguet_msgfmt_options
            --check
            --check-format
            --check-domain
        )
    else()
        set(_moguet_msgfmt_options --check-header --check-domain)
    endif()

    add_custom_command(
        OUTPUT "${output_file}"
        COMMAND
            "${CMAKE_COMMAND}" -E make_directory
            "${_moguet_catalog_directory}"
        COMMAND
            "${CMAKE_COMMAND}" -E rm -f
            "${_moguet_catalog_temporary_file}"
        COMMAND
            "${MOGUET_MSGFMT_EXECUTABLE}"
            ${_moguet_msgfmt_options}
            "--output-file=${_moguet_catalog_temporary_file}"
            "${input_file}"
        COMMAND
            "${CMAKE_COMMAND}" -E rename
            "${_moguet_catalog_temporary_file}"
            "${output_file}"
        DEPENDS "${input_file}"
        COMMENT "Compiling message catalog ${input_file}"
        VERBATIM
    )
endfunction()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/po/LINGUAS" MOGUET_LINGUAS)
list(FILTER MOGUET_LINGUAS EXCLUDE REGEX "^[ \t]*(#|$)")
if(NOT MOGUET_LINGUAS)
    message(FATAL_ERROR "po/LINGUAS must declare at least one locale")
endif()

set(MOGUET_CATALOG_FILES "")
foreach(_moguet_locale IN LISTS MOGUET_LINGUAS)
    string(STRIP "${_moguet_locale}" _moguet_locale)
    if(NOT _moguet_locale MATCHES "^[A-Za-z0-9_.@-]+$")
        message(FATAL_ERROR "Invalid locale token in po/LINGUAS: ${_moguet_locale}")
    endif()

    set(_moguet_catalog_source "${CMAKE_CURRENT_SOURCE_DIR}/po/${_moguet_locale}.po")
    if(NOT EXISTS "${_moguet_catalog_source}")
        message(FATAL_ERROR "Missing catalog source: ${_moguet_catalog_source}")
    endif()

    set(
        _moguet_catalog_file
        "${CMAKE_CURRENT_BINARY_DIR}/locale/${_moguet_locale}/LC_MESSAGES/moguet.mo"
    )
    moguet_add_catalog(
        "${_moguet_catalog_file}"
        "${_moguet_catalog_source}"
        TRUE
    )
    list(APPEND MOGUET_CATALOG_FILES "${_moguet_catalog_file}")
    if(_moguet_locale STREQUAL "ja")
        set(MOGUET_JA_CATALOG "${_moguet_catalog_file}")
    endif()
endforeach()

if(NOT MOGUET_JA_CATALOG)
    message(FATAL_ERROR "po/LINGUAS must retain the supported ja catalog")
endif()

add_custom_target(moguet_catalogs ALL DEPENDS ${MOGUET_CATALOG_FILES})

# CMake does not consume CPPFLAGS.  Match the current-invocation semantics
# only for an opted-in frontend.  Direct CMake initializes the cache once and
# preserves an explicit -DMOGUET_CPPFLAGS value over the environment.
if(_moguet_sync_external_build_inputs)
    set(
        MOGUET_CPPFLAGS
        "$ENV{CPPFLAGS}"
        CACHE STRING
        "External preprocessor flags synchronized from CPPFLAGS"
        FORCE
    )
elseif(NOT DEFINED CACHE{MOGUET_CPPFLAGS})
    set(
        MOGUET_CPPFLAGS
        "$ENV{CPPFLAGS}"
        CACHE STRING
        "External preprocessor flags initialized from CPPFLAGS"
    )
endif()

function(
    moguet_parse_external_compile_flags
    output_variable
    input_name
    raw_flags
)
    separate_arguments(_moguet_arguments UNIX_COMMAND "${raw_flags}")

    set(_moguet_pending_macro_option "")
    foreach(_moguet_argument IN LISTS _moguet_arguments)
        if(_moguet_argument MATCHES "^--?std=")
            message(
                FATAL_ERROR
                "External ${input_name} contains a C/C++ standard selector "
                "(-std=... or --std=...), which conflicts with Moguet's "
                "reserved strict C++20 contract. Remove the external "
                "standard selector."
            )
        endif()

        if(
            _moguet_argument MATCHES
                "^-DMOGUET_VERSION($|[^A-Za-z0-9_])"
            OR _moguet_argument MATCHES
                "^-UMOGUET_VERSION($|[^A-Za-z0-9_])"
        )
            message(
                FATAL_ERROR
                "External ${input_name} defines or undefines MOGUET_VERSION, "
                "which is reserved for the repository root VERSION file. "
                "Remove the external MOGUET_VERSION -D or -U option."
            )
        endif()

        if(
            _moguet_argument MATCHES
                "^-DMOGUET_ALPM_RECEIPT_HELPER_PATH($|[^A-Za-z0-9_])"
            OR _moguet_argument MATCHES
                "^-UMOGUET_ALPM_RECEIPT_HELPER_PATH($|[^A-Za-z0-9_])"
        )
            message(
                FATAL_ERROR
                "External ${input_name} defines or undefines "
                "MOGUET_ALPM_RECEIPT_HELPER_PATH, which is reserved for "
                "the configured package install graph. Remove the external "
                "helper-path -D or -U option."
            )
        endif()

        if(NOT _moguet_pending_macro_option STREQUAL "")
            if(
                _moguet_argument MATCHES
                    "^MOGUET_VERSION($|[^A-Za-z0-9_])"
            )
                message(
                    FATAL_ERROR
                    "External ${input_name} defines or undefines "
                    "MOGUET_VERSION, which is reserved for the repository "
                    "root VERSION file. Remove the external MOGUET_VERSION "
                    "-D or -U option."
                )
            endif()
            if(
                _moguet_argument MATCHES
                    "^MOGUET_ALPM_RECEIPT_HELPER_PATH($|[^A-Za-z0-9_])"
            )
                message(
                    FATAL_ERROR
                    "External ${input_name} defines or undefines "
                    "MOGUET_ALPM_RECEIPT_HELPER_PATH, which is reserved "
                    "for the configured package install graph. Remove the "
                    "external helper-path -D or -U option."
                )
            endif()
            set(_moguet_pending_macro_option "")
        endif()

        if(_moguet_argument STREQUAL "-D" OR _moguet_argument STREQUAL "-U")
            set(_moguet_pending_macro_option "${_moguet_argument}")
        endif()
    endforeach()

    # Keep every parsed argv element in one SHELL: group so CMake's compile
    # option de-duplication cannot collapse repeated tokens such as
    # `-D A=1 -D B=2`. This only re-encodes argv for CMake's parser; no shell
    # process is invoked.
    set(_moguet_shell_group "")
    foreach(_moguet_argument IN LISTS _moguet_arguments)
        string(REPLACE "\\" "\\\\" _moguet_quoted "${_moguet_argument}")
        string(REPLACE "'" "'\\''" _moguet_quoted "${_moguet_quoted}")
        if(_moguet_shell_group STREQUAL "")
            set(_moguet_shell_group "'${_moguet_quoted}'")
        else()
            string(APPEND _moguet_shell_group " '${_moguet_quoted}'")
        endif()
    endforeach()

    if(_moguet_shell_group STREQUAL "")
        set("${output_variable}" "" PARENT_SCOPE)
    else()
        # A literal semicolon must remain escaped until the SHELL: group reaches
        # target_compile_options() as one CMake list element.
        string(REPLACE ";" "\\;" _moguet_shell_group "${_moguet_shell_group}")
        set(
            "${output_variable}"
            "SHELL:${_moguet_shell_group}"
            PARENT_SCOPE
        )
    endif()
endfunction()

moguet_parse_external_compile_flags(
    _moguet_cppflag_option_group
    "MOGUET_CPPFLAGS (initialized from CPPFLAGS)"
    "${MOGUET_CPPFLAGS}"
)
moguet_parse_external_compile_flags(
    _moguet_cxxflag_option_group
    "CMAKE_CXX_FLAGS (including CXXFLAGS initialization)"
    "${CMAKE_CXX_FLAGS}"
)

add_library(moguet_external_cppflags INTERFACE)
if(NOT _moguet_cppflag_option_group STREQUAL "")
    target_compile_options(
        moguet_external_cppflags
        INTERFACE
        "${_moguet_cppflag_option_group}"
    )
endif()

if(_moguet_sync_external_build_inputs)
    # Make communicates whether its synchronized empty CXXFLAGS was undefined
    # (use CMake's defaults) or explicitly empty (suppress those defaults).
    # Other synchronized frontends retain the explicit-empty behavior unless
    # they opt in to the same invocation-scoped distinction.
    set(
        MOGUET_ENABLE_DEFAULT_COMPILE_OPTIONS
        "${_moguet_frontend_use_default_compile_options}"
        CACHE BOOL
        "Use Moguet's -O2 and -pipe defaults for a plain developer build"
        FORCE
    )
elseif(NOT DEFINED CACHE{MOGUET_ENABLE_DEFAULT_COMPILE_OPTIONS})
    if(_moguet_cxxflags_defined_before_project)
        set(_moguet_enable_default_compile_options OFF)
    else()
        set(_moguet_enable_default_compile_options ON)
    endif()
    set(
        MOGUET_ENABLE_DEFAULT_COMPILE_OPTIONS
        "${_moguet_enable_default_compile_options}"
        CACHE BOOL
        "Use Moguet's -O2 and -pipe defaults for a plain developer build"
    )
endif()

add_library(moguet_build_contract INTERFACE)
target_compile_features(moguet_build_contract INTERFACE cxx_std_20)
target_compile_options(moguet_build_contract INTERFACE -Wall -Wextra)
if(
    MOGUET_ENABLE_DEFAULT_COMPILE_OPTIONS
    AND CMAKE_CXX_FLAGS STREQUAL ""
    AND CMAKE_BUILD_TYPE STREQUAL ""
    AND NOT CMAKE_CONFIGURATION_TYPES
)
    target_compile_options(moguet_build_contract INTERFACE -O2 -pipe)
endif()
target_compile_definitions(
    moguet_build_contract
    INTERFACE
    "MOGUET_VERSION=\"${MOGUET_VERSION}\""
    "MOGUET_ALPM_RECEIPT_HELPER_PATH=\"${MOGUET_ALPM_RECEIPT_HELPER_INSTALL_PATH}\""
    "MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH=\"${MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_INSTALL_PATH}\""
)
target_include_directories(
    moguet_build_contract
    INTERFACE
    "${MOGUET_GENERATED_INCLUDE_DIR}"
)

# Arch's tomlplusplus package also exports a shared-library target. Moguet's
# current production contract is header-only, so expose includes and no link
# dependency or TOML_HEADER_ONLY/TOML_SHARED_LIB definition changes.
add_library(moguet_toml_headers INTERFACE)
target_include_directories(
    moguet_toml_headers
    INTERFACE
    "${TOMLPLUSPLUS_INCLUDE_DIR}"
)

# Production compilation needs libalpm's pkg-config usage requirements, while
# the final executable owns the real libalpm link. Keeping these separate
# leaves future ALPM-stub test profiles free of the real library dependency.
add_library(moguet_alpm_compile_contract INTERFACE)
if(ALPM_INCLUDE_DIRS)
    target_include_directories(
        moguet_alpm_compile_contract
        INTERFACE
        ${ALPM_INCLUDE_DIRS}
    )
endif()

add_executable(
    moguet-alpm-receipt-helper
    ${MOGUET_ALPM_RECEIPT_HELPER_SOURCES}
)
add_executable(
    moguet-source-artifact-install-helper
    ${MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_SOURCES}
)
set_target_properties(
    moguet-source-artifact-install-helper
    PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)
target_link_libraries(
    moguet-source-artifact-install-helper
    PRIVATE
        moguet_external_cppflags
        moguet_build_contract
        PkgConfig::ALPM
)
target_include_directories(
    moguet-source-artifact-install-helper
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source"
)
set_target_properties(
    moguet-alpm-receipt-helper
    PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)
target_link_libraries(
    moguet-alpm-receipt-helper
    PRIVATE
        moguet_external_cppflags
        moguet_build_contract
)
target_include_directories(
    moguet-alpm-receipt-helper
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source"
)
if(ALPM_CFLAGS_OTHER)
    target_compile_options(
        moguet_alpm_compile_contract
        INTERFACE
        ${ALPM_CFLAGS_OTHER}
    )
endif()

# These two translation units have undefined-symbol firewalls. Keep the exact
# objects consumed by production addressable so CTest can inspect them without
# substituting a broad archive, a final executable, or a separately compiled
# probe object.
set(
    MOGUET_UNIFIED_PLAN_PROJECTION_SOURCE
    source/unified_plan_projection.cpp
)
set(
    MOGUET_UNIFIED_PLAN_RENDERER_SOURCE
    source/unified_plan_renderer.cpp
)
set(
    MOGUET_ADDRESSABLE_PRODUCTION_SOURCES
    "${MOGUET_UNIFIED_PLAN_PROJECTION_SOURCE}"
    "${MOGUET_UNIFIED_PLAN_RENDERER_SOURCE}"
)
set(
    _moguet_unique_addressable_production_sources
    ${MOGUET_ADDRESSABLE_PRODUCTION_SOURCES}
)
list(REMOVE_DUPLICATES _moguet_unique_addressable_production_sources)
list(
    LENGTH
    MOGUET_ADDRESSABLE_PRODUCTION_SOURCES
    _moguet_addressable_production_source_count
)
list(
    LENGTH
    _moguet_unique_addressable_production_sources
    _moguet_unique_addressable_production_source_count
)
if(
    NOT _moguet_addressable_production_source_count EQUAL 2
    OR NOT _moguet_addressable_production_source_count EQUAL
        _moguet_unique_addressable_production_source_count
)
    message(
        FATAL_ERROR
        "MOGUET_ADDRESSABLE_PRODUCTION_SOURCES must contain exactly two "
        "unique translation units"
    )
endif()
foreach(
    _moguet_addressable_production_source
    IN LISTS MOGUET_ADDRESSABLE_PRODUCTION_SOURCES
)
    list(
        FIND
        MOGUET_PRODUCTION_SOURCES
        "${_moguet_addressable_production_source}"
        _moguet_addressable_production_source_index
    )
    if(_moguet_addressable_production_source_index EQUAL -1)
        message(
            FATAL_ERROR
            "Addressable production source is absent from the canonical "
            "manifest: ${_moguet_addressable_production_source}"
        )
    endif()
endforeach()

set(MOGUET_PRODUCTION_OBJECT_SOURCES ${MOGUET_PRODUCTION_SOURCES})
list(
    REMOVE_ITEM
    MOGUET_PRODUCTION_OBJECT_SOURCES
    ${MOGUET_ADDRESSABLE_PRODUCTION_SOURCES}
)
set(
    _moguet_partitioned_production_sources
    ${MOGUET_PRODUCTION_OBJECT_SOURCES}
    ${MOGUET_ADDRESSABLE_PRODUCTION_SOURCES}
)
set(
    _moguet_unique_partitioned_production_sources
    ${_moguet_partitioned_production_sources}
)
list(REMOVE_DUPLICATES _moguet_unique_partitioned_production_sources)
list(
    LENGTH
    _moguet_partitioned_production_sources
    _moguet_partitioned_production_source_count
)
list(
    LENGTH
    _moguet_unique_partitioned_production_sources
    _moguet_unique_partitioned_production_source_count
)
if(
    NOT _moguet_partitioned_production_source_count EQUAL
        _moguet_unique_partitioned_production_source_count
)
    message(FATAL_ERROR "Production OBJECT source partition overlaps")
endif()
set(_moguet_sorted_production_sources ${MOGUET_PRODUCTION_SOURCES})
set(
    _moguet_sorted_partitioned_production_sources
    ${_moguet_partitioned_production_sources}
)
list(SORT _moguet_sorted_production_sources)
list(SORT _moguet_sorted_partitioned_production_sources)
if(
    NOT "${_moguet_sorted_production_sources}" STREQUAL
        "${_moguet_sorted_partitioned_production_sources}"
)
    message(
        FATAL_ERROR
        "Production OBJECT sources are not an exact partition of "
        "MOGUET_PRODUCTION_SOURCES"
    )
endif()

add_library(
    moguet_production_objects
    OBJECT
    ${MOGUET_PRODUCTION_OBJECT_SOURCES}
)
add_library(
    moguet_unified_plan_projection_object
    OBJECT
    "${MOGUET_UNIFIED_PLAN_PROJECTION_SOURCE}"
)
add_library(
    moguet_unified_plan_renderer_object
    OBJECT
    "${MOGUET_UNIFIED_PLAN_RENDERER_SOURCE}"
)

function(moguet_configure_production_object_target target_name)
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
            moguet_alpm_compile_contract
            # Consume CURL::libcurl's complete compile usage requirements.
            # The executable remains the explicit actual curl link owner.
            CURL::libcurl
            nlohmann_json::nlohmann_json
            moguet_toml_headers
    )
endfunction()

moguet_configure_production_object_target(moguet_production_objects)
moguet_configure_production_object_target(
    moguet_unified_plan_projection_object
)
moguet_configure_production_object_target(
    moguet_unified_plan_renderer_object
)

# Raw TARGET_OBJECTS consumption does not transfer the OBJECT target's PRIVATE
# curl dependency to the final link, so keep the actual linkage explicit.
add_executable(
    moguet
    $<TARGET_OBJECTS:moguet_production_objects>
    $<TARGET_OBJECTS:moguet_unified_plan_projection_object>
    $<TARGET_OBJECTS:moguet_unified_plan_renderer_object>
)
target_link_libraries(
    moguet
    PRIVATE
        CURL::libcurl
        PkgConfig::ALPM
)
add_dependencies(
    moguet
    moguet_catalogs
    moguet-alpm-receipt-helper
    moguet-source-artifact-install-helper
)

# Completion generation consumes a small C++ projection helper.  Keep its
# compile/link graph in CMake and exclude it from ordinary production/package
# builds; repository validation requests the target explicitly.
add_executable(
    moguet-cli-authority-exporter
    EXCLUDE_FROM_ALL
    scripts/export_cli_authority.cpp
    source/cli_public_projection.cpp
)
set_target_properties(
    moguet-cli-authority-exporter
    PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)
target_link_libraries(
    moguet-cli-authority-exporter
    PRIVATE
        moguet_external_cppflags
        moguet_build_contract
)
target_include_directories(
    moguet-cli-authority-exporter
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source"
)

# Tracked completion writes stay behind the CMake dependency graph.  The
# Python process only renders to stdout; this target publishes its output
# after CMake has brought the exporter up to date for the current sources.
add_custom_target(
    moguet-generate-completions
    COMMAND
        "${CMAKE_COMMAND}"
        "-DMOGUET_COMPLETION_PYTHON=python3"
        "-DMOGUET_COMPLETION_GENERATOR=${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_completions.py"
        "-DMOGUET_COMPLETION_EXPORTER=$<TARGET_FILE:moguet-cli-authority-exporter>"
        "-DMOGUET_COMPLETION_OUTPUT_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/completions"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MoguetGenerateCompletions.cmake"
    COMMENT "Generating tracked shell completions from current CLI authority"
    USES_TERMINAL
    VERBATIM
)
add_dependencies(
    moguet-generate-completions
    moguet-cli-authority-exporter
)

install(TARGETS moguet RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(
    TARGETS moguet-alpm-receipt-helper
    # The executable destination and compiled hook Exec path are one security
    # authority. An install-time --prefix must not relocate only the file.
    RUNTIME DESTINATION "${MOGUET_FULL_INTERNAL_EXECUTABLE_DIRECTORY}"
    PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE
)
install(
    TARGETS moguet-source-artifact-install-helper
    # This executable is a separate fixed SourceArtifactInstall authority.
    # It shares neither the selected-provider verb grammar nor its state root.
    RUNTIME DESTINATION "${MOGUET_FULL_INTERNAL_EXECUTABLE_DIRECTORY}"
    PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE
)
foreach(_moguet_locale _moguet_catalog_file IN ZIP_LISTS MOGUET_LINGUAS MOGUET_CATALOG_FILES)
    install(
        FILES "${_moguet_catalog_file}"
        DESTINATION "${MOGUET_LOCALE_DIRECTORY}/${_moguet_locale}/LC_MESSAGES"
    )
endforeach()

set(
    MOGUET_INSTALL_BASH_COMPLETION_DIRECTORY
    "/usr/share/bash-completion/completions"
    CACHE PATH
    "Installation directory for the bash completion"
)
set(
    MOGUET_INSTALL_ZSH_COMPLETION_DIRECTORY
    "/usr/share/zsh/site-functions"
    CACHE PATH
    "Installation directory for the zsh completion"
)
set(
    MOGUET_INSTALL_FISH_COMPLETION_DIRECTORY
    "/usr/share/fish/vendor_completions.d"
    CACHE PATH
    "Installation directory for the fish completion"
)
set(
    MOGUET_INSTALL_MAN1_DIRECTORY
    "${CMAKE_INSTALL_MANDIR}/man1"
    CACHE PATH
    "Installation directory for the English man page"
)
set(
    MOGUET_INSTALL_JA_MAN1_DIRECTORY
    "${CMAKE_INSTALL_MANDIR}/ja/man1"
    CACHE PATH
    "Installation directory for the Japanese man page"
)
set(
    MOGUET_INSTALL_LICENSE_DIRECTORY
    "share/licenses/moguet"
    CACHE PATH
    "Installation directory for Moguet license files"
)
set(
    MOGUET_INSTALL_DOCUMENT_DIRECTORY
    "share/doc/moguet"
    CACHE PATH
    "Installation directory for Moguet documentation"
)

# Completion and locale defaults intentionally retain the legacy Arch package
# contract and therefore do not follow a non-/usr development prefix.  The
# cache variables keep Make destination overrides inside this install graph.
install(
    FILES completions/moguet.bash
    DESTINATION "${MOGUET_INSTALL_BASH_COMPLETION_DIRECTORY}"
    RENAME moguet
)
install(
    FILES completions/_moguet
    DESTINATION "${MOGUET_INSTALL_ZSH_COMPLETION_DIRECTORY}"
)
install(
    FILES completions/moguet.fish
    DESTINATION "${MOGUET_INSTALL_FISH_COMPLETION_DIRECTORY}"
)
install(FILES man/moguet.1 DESTINATION "${MOGUET_INSTALL_MAN1_DIRECTORY}")
install(FILES man/ja/moguet.1 DESTINATION "${MOGUET_INSTALL_JA_MAN1_DIRECTORY}")

install(
    FILES
        LICENSE
        LICENSES/jpacker-MIT-legacy.txt
        LICENSES/curl.txt
        LICENSES/nlohmann-json-MIT.txt
        LICENSES/tomlplusplus-MIT.txt
        LICENSES/bjoern-hoehrmann-utf8-MIT.txt
    DESTINATION "${MOGUET_INSTALL_LICENSE_DIRECTORY}"
)

install(
    FILES
        README.md
        README.ja.md
        THIRD_PARTY_NOTICES.md
    DESTINATION "${MOGUET_INSTALL_DOCUMENT_DIRECTORY}"
)
install(
    FILES sample/config.toml
    DESTINATION "${MOGUET_INSTALL_DOCUMENT_DIRECTORY}/examples"
)
install(
    FILES docs/LICENSING.md
    DESTINATION "${MOGUET_INSTALL_DOCUMENT_DIRECTORY}/docs"
)
install(
    FILES
        docs/migration/v1-to-v2.md
        docs/migration/v1-to-v2.ja.md
    DESTINATION "${MOGUET_INSTALL_DOCUMENT_DIRECTORY}/docs/migration"
)

add_executable(
    moguet-uninstall-helper
    EXCLUDE_FROM_ALL
    cmake/MoguetUninstall.cpp
)
set_target_properties(
    moguet-uninstall-helper
    PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)
target_link_libraries(
    moguet-uninstall-helper
    PRIVATE
        moguet_external_cppflags
        moguet_build_contract
)

function(moguet_resolve_install_root output_variable destination)
    if(IS_ABSOLUTE "${destination}")
        set(_moguet_install_root "${destination}")
    else()
        set(
            _moguet_install_root
            "${CMAKE_INSTALL_PREFIX}/${destination}"
        )
    endif()
    set("${output_variable}" "${_moguet_install_root}" PARENT_SCOPE)
endfunction()

set(_moguet_uninstall_roots "")
foreach(
    _moguet_install_destination
    IN ITEMS
        "${CMAKE_INSTALL_BINDIR}"
        "${MOGUET_FULL_INTERNAL_EXECUTABLE_DIRECTORY}"
        "${MOGUET_INSTALL_BASH_COMPLETION_DIRECTORY}"
        "${MOGUET_INSTALL_ZSH_COMPLETION_DIRECTORY}"
        "${MOGUET_INSTALL_FISH_COMPLETION_DIRECTORY}"
        "${MOGUET_INSTALL_MAN1_DIRECTORY}"
        "${MOGUET_INSTALL_JA_MAN1_DIRECTORY}"
        "${MOGUET_INSTALL_LICENSE_DIRECTORY}"
        "${MOGUET_INSTALL_DOCUMENT_DIRECTORY}"
        "${MOGUET_LOCALE_DIRECTORY}"
)
    moguet_resolve_install_root(
        _moguet_resolved_install_root
        "${_moguet_install_destination}"
    )
    list(APPEND _moguet_uninstall_roots "${_moguet_resolved_install_root}")
endforeach()
list(REMOVE_DUPLICATES _moguet_uninstall_roots)

set(_moguet_uninstall_root_arguments "")
foreach(_moguet_uninstall_root IN LISTS _moguet_uninstall_roots)
    list(
        APPEND
        _moguet_uninstall_root_arguments
        --allowed-root
        "${_moguet_uninstall_root}"
    )
endforeach()

add_custom_target(
    uninstall
    COMMAND
        "$<TARGET_FILE:moguet-uninstall-helper>"
        --manifest "${CMAKE_CURRENT_BINARY_DIR}/install_manifest.txt"
        ${_moguet_uninstall_root_arguments}
    DEPENDS moguet-uninstall-helper
    COMMENT "Removing files recorded by the canonical CMake install manifest"
    USES_TERMINAL
    VERBATIM
)

if(BUILD_TESTING)
    include(cmake/MoguetTests.cmake)
endif()
