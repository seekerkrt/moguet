# Keep the CMake 3.18 minimum and adopt only reviewed build semantics.
# Include with NO_POLICY_SCOPE before project()/target creation.
if(POLICY CMP0200)
    # Imported configurations are authoritative; current CURL locations and
    # configuration-independent ALPM/JSON usage requirements remain unchanged.
    cmake_policy(SET CMP0200 NEW)
endif()

if(POLICY CMP0156)
    # Preserve static-library repetition and last-occurrence shared-library
    # ordering, including when an external linker override is supplied.
    cmake_policy(SET CMP0156 OLD)
endif()

if(POLICY CMP0181)
    # External LDFLAGS/cache inputs retain the CMake 3.18 command-fragment
    # contract. Do not silently re-quote them or reinterpret LINKER: tokens.
    cmake_policy(SET CMP0181 OLD)
endif()
