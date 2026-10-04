cmake_minimum_required(VERSION 3.22)

if (NOT DEFINED JUCE_SOURCE_DIR OR "${JUCE_SOURCE_DIR}" STREQUAL "")
    message(FATAL_ERROR "JUCE_SOURCE_DIR is required")
endif()

set(juce_root_cmake "${JUCE_SOURCE_DIR}/CMakeLists.txt")
set(module_support "${JUCE_SOURCE_DIR}/extras/Build/CMake/JUCEModuleSupport.cmake")
set(juceaide_cmake "${JUCE_SOURCE_DIR}/extras/Build/juceaide/CMakeLists.txt")

foreach(required_file IN ITEMS "${juce_root_cmake}" "${module_support}" "${juceaide_cmake}")
    if (NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Expected pinned JUCE file is missing: ${required_file}")
    endif()
endforeach()

# These hashes describe the reviewed JUCE 9.0.0 commit before and after this
# compatibility transform. The repository stores only hashes, short unique
# markers, and first-party replacement logic, not JUCE implementation blocks.
set(root_original_sha256   "8e93e5b56c9d307c08ef5c5afe323408534c2ec721e07ea2d562fc5e586e5005")
set(root_patched_sha256    "7a21685720789545410b5e5d886f2531a4b608e4f049887fb30ad842c7083b2e")
set(module_original_sha256 "c518f7273bc665f99be96761b97a25d67065be484c59f11e697a2cac5fd8e0cd")
set(module_patched_sha256  "5f94cc6b14cb610be9af2ddac20400dd65073492a9be95f19928a61a3f1f90a8")
set(aide_original_sha256   "d7c583760eb6827b16d99c5d45c280dd67cac8829fc118de076bbfd14ee4f1ef")
set(aide_patched_sha256    "b85e3b21e01e920222fc20e5ae999d87e60c0958fb3442377c121f9000c9e150")

function(read_normalized_and_hash path contents_output hash_output)
    file(READ "${path}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    string(SHA256 digest "${contents}")
    set(${contents_output} "${contents}" PARENT_SCOPE)
    set(${hash_output} "${digest}" PARENT_SCOPE)
endfunction()

function(require_known_state label current_hash original_hash patched_hash)
    if (NOT current_hash STREQUAL original_hash AND NOT current_hash STREQUAL patched_hash)
        message(FATAL_ERROR
            "Unrecognised pinned JUCE ${label} source (${current_hash}); review the compatibility patch before changing the dependency")
    endif()
endfunction()

function(count_literal text needle result)
    string(LENGTH "${text}" original_length)
    string(REPLACE "${needle}" "" without_needle "${text}")
    string(LENGTH "${without_needle}" shortened_length)
    string(LENGTH "${needle}" needle_length)
    math(EXPR occurrences "(${original_length} - ${shortened_length}) / ${needle_length}")
    set(${result} "${occurrences}" PARENT_SCOPE)
endfunction()

read_normalized_and_hash("${juce_root_cmake}" root_original root_current_sha256)
read_normalized_and_hash("${module_support}" module_original module_current_sha256)
read_normalized_and_hash("${juceaide_cmake}" aide_original aide_current_sha256)
require_known_state("root CMake" "${root_current_sha256}"
    "${root_original_sha256}" "${root_patched_sha256}")
require_known_state("module support" "${module_current_sha256}"
    "${module_original_sha256}" "${module_patched_sha256}")
require_known_state("juceaide CMake" "${aide_current_sha256}"
    "${aide_original_sha256}" "${aide_patched_sha256}")

set(root_updated "${root_original}")
if (root_current_sha256 STREQUAL root_original_sha256)
    set(root_project_original "project(JUCE VERSION 9.0.0 LANGUAGES C CXX)")
    set(root_project_patched  "project(JUCE VERSION 9.0.0 LANGUAGES CXX)")
    count_literal("${root_updated}" "${root_project_original}" root_project_count)
    if (NOT root_project_count EQUAL 1)
        message(FATAL_ERROR "Pinned JUCE root project marker is not unique")
    endif()
    string(REPLACE "${root_project_original}" "${root_project_patched}"
        root_updated "${root_updated}")

    set(root_guard_start [=[if(NOT (JUCE_IS_TOP_LEVEL OR "C" IN_LIST _juce_initial_global_languages))]=])
    string(FIND "${root_updated}" "${root_guard_start}" root_guard_start_index)
    if (root_guard_start_index LESS 0)
        message(FATAL_ERROR "Pinned JUCE C-compiler guard start was not found")
    endif()
    string(SUBSTRING "${root_updated}" ${root_guard_start_index} -1 root_guard_tail)
    string(FIND "${root_guard_tail}" "\nendif()\n" root_guard_end_relative)
    if (root_guard_end_relative LESS 0)
        message(FATAL_ERROR "Pinned JUCE C-compiler guard end was not found")
    endif()
    string(LENGTH "\nendif()\n" root_guard_end_marker_length)
    math(EXPR root_guard_length "${root_guard_end_relative} + ${root_guard_end_marker_length}")
    string(SUBSTRING "${root_guard_tail}" 0 ${root_guard_length} root_guard)
    string(FIND "${root_guard}" "A C compiler is required" root_guard_message_index)
    if (root_guard_message_index LESS 0 OR root_guard_length GREATER 400)
        message(FATAL_ERROR "Pinned JUCE C-compiler guard did not match the reviewed bounded block")
    endif()
    string(SUBSTRING "${root_updated}" 0 ${root_guard_start_index} root_prefix)
    math(EXPR root_after_guard_index "${root_guard_start_index} + ${root_guard_length}")
    string(SUBSTRING "${root_updated}" ${root_after_guard_index} -1 root_suffix)
    set(root_updated "${root_prefix}${root_suffix}")
endif()

set(module_updated "${module_original}")
if (module_current_sha256 STREQUAL module_original_sha256)
    set(module_marker [=[    set(${built_sources} ${module_files_to_build} PARENT_SCOPE)]=])
    count_literal("${module_updated}" "${module_marker}" module_marker_count)
    if (NOT module_marker_count EQUAL 1)
        message(FATAL_ERROR "Pinned JUCE module insertion marker is not unique")
    endif()
    set(module_insertion [=[    # Local build workaround: the installed CMake/MSVC combination crashes
    # while enabling C and CXX together. Generate C++ wrappers for JUCE's C
    # translation units for this verification build.
    set(rewritten_module_files)
    foreach(source IN LISTS module_files_to_build)
        if(source MATCHES "\\.c$")
            string(MD5 source_hash "${source}")
            set(wrapper "${CMAKE_BINARY_DIR}/juce_cxx_wrappers/${source_hash}.cpp")
            file(WRITE "${wrapper}" "#include \"${source}\"\n")
            set_property(GLOBAL APPEND PROPERTY JUCE_C_WRAPPERS "${wrapper}")
            list(APPEND rewritten_module_files "${wrapper}")
        else()
            list(APPEND rewritten_module_files "${source}")
        endif()
    endforeach()
    set(module_files_to_build ${rewritten_module_files})]=])
    string(REPLACE "${module_marker}" "${module_insertion}\n\n${module_marker}"
        module_updated "${module_updated}")
endif()

set(aide_updated "${aide_original}")
if (aide_current_sha256 STREQUAL aide_original_sha256)
    set(aide_marker
        "    set_target_properties(juceaide PROPERTIES MSVC_RUNTIME_LIBRARY \"MultiThreaded\")")
    count_literal("${aide_updated}" "${aide_marker}" aide_marker_count)
    if (NOT aide_marker_count EQUAL 1)
        message(FATAL_ERROR "Pinned JUCE juceaide insertion marker is not unique")
    endif()
    set(aide_insertion
        "    get_property(juce_c_wrappers GLOBAL PROPERTY JUCE_C_WRAPPERS)\n    set_source_files_properties(\${juce_c_wrappers} PROPERTIES COMPILE_OPTIONS \"/TC\")")
    string(REPLACE "${aide_marker}" "${aide_insertion}\n\n${aide_marker}"
        aide_updated "${aide_updated}")
endif()

string(SHA256 root_updated_sha256 "${root_updated}")
string(SHA256 module_updated_sha256 "${module_updated}")
string(SHA256 aide_updated_sha256 "${aide_updated}")
if (NOT root_updated_sha256 STREQUAL root_patched_sha256
    OR NOT module_updated_sha256 STREQUAL module_patched_sha256
    OR NOT aide_updated_sha256 STREQUAL aide_patched_sha256)
    message(FATAL_ERROR
        "JUCE CXX-only compatibility transform did not produce the reviewed patched hashes\n"
        "  root:   ${root_updated_sha256} (expected ${root_patched_sha256})\n"
        "  module: ${module_updated_sha256} (expected ${module_patched_sha256})\n"
        "  aide:   ${aide_updated_sha256} (expected ${aide_patched_sha256})")
endif()

# All three outputs were verified before any write, preventing a partial patch.
if (root_current_sha256 STREQUAL root_original_sha256)
    file(WRITE "${juce_root_cmake}" "${root_updated}")
endif()
if (module_current_sha256 STREQUAL module_original_sha256)
    file(WRITE "${module_support}" "${module_updated}")
endif()
if (aide_current_sha256 STREQUAL aide_original_sha256)
    file(WRITE "${juceaide_cmake}" "${aide_updated}")
endif()

message(STATUS "Pinned JUCE CXX-only compatibility patch verified")
