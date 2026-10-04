cmake_minimum_required(VERSION 3.22)

if (NOT DEFINED JUCE_SOURCE_DIR OR "${JUCE_SOURCE_DIR}" STREQUAL "")
    message(FATAL_ERROR "JUCE_SOURCE_DIR is required for the WASAPI session patch")
endif()

set(wasapi_source "${JUCE_SOURCE_DIR}/modules/juce_audio_devices/native/juce_WASAPI_windows.cpp")
if (NOT EXISTS "${wasapi_source}" OR IS_DIRECTORY "${wasapi_source}")
    message(FATAL_ERROR "Expected pinned JUCE WASAPI source is missing: ${wasapi_source}")
endif()

# The dependency is pinned to one reviewed JUCE commit. Hash the complete
# normalised source before and after applying two minimal token changes, so an
# upstream or local edit is rejected without storing JUCE implementation text
# in this MIT-licensed repository.
set(wasapi_original_sha256 "2083d9847c743d50deb16c519407b4f62ebfe609335397e24f1c224a93f4c3b3")
set(wasapi_patched_sha256  "e546d96a290c2cb7808561d4f30d6dacfe5cffd3ace318b6bbc629a2ffc24094")

set(wasapi_function_start "    bool initialiseStandardClient (")
set(wasapi_function_end   "    bool tryInitialisingWithBufferSize (")
set(wasapi_session_declaration "            GUID session;\n")
set(wasapi_original_argument   "                                          &session);")
set(wasapi_patched_argument    "                                          nullptr);")

function(count_literal text needle result)
    string(LENGTH "${text}" original_length)
    string(REPLACE "${needle}" "" without_needle "${text}")
    string(LENGTH "${without_needle}" shortened_length)
    string(LENGTH "${needle}" needle_length)
    math(EXPR occurrences "(${original_length} - ${shortened_length}) / ${needle_length}")
    set(${result} "${occurrences}" PARENT_SCOPE)
endfunction()

file(READ "${wasapi_source}" wasapi_raw)
string(REPLACE "\r\n" "\n" wasapi_normalized "${wasapi_raw}")
string(SHA256 wasapi_current_sha256 "${wasapi_normalized}")

if (wasapi_current_sha256 STREQUAL wasapi_patched_sha256)
    message(STATUS "JUCE WASAPI default-session patch already applied")
    return()
endif()

if (NOT wasapi_current_sha256 STREQUAL wasapi_original_sha256)
    message(FATAL_ERROR
        "Unrecognised pinned JUCE WASAPI source (${wasapi_current_sha256}); review the session patch before changing the dependency")
endif()

count_literal("${wasapi_normalized}" "${wasapi_function_start}" wasapi_start_count)
count_literal("${wasapi_normalized}" "${wasapi_function_end}" wasapi_end_count)
if (NOT wasapi_start_count EQUAL 1 OR NOT wasapi_end_count EQUAL 1)
    message(FATAL_ERROR "Pinned JUCE WASAPI function markers are not unique")
endif()

string(FIND "${wasapi_normalized}" "${wasapi_function_start}" wasapi_start_index)
string(FIND "${wasapi_normalized}" "${wasapi_function_end}" wasapi_end_index)
if (wasapi_start_index LESS 0 OR wasapi_end_index LESS_EQUAL wasapi_start_index)
    message(FATAL_ERROR "Pinned JUCE WASAPI standard-client function boundaries are invalid")
endif()

math(EXPR wasapi_function_length "${wasapi_end_index} - ${wasapi_start_index}")
string(SUBSTRING "${wasapi_normalized}" ${wasapi_start_index}
    ${wasapi_function_length} wasapi_function)
count_literal("${wasapi_function}" "${wasapi_session_declaration}" wasapi_declaration_count)
count_literal("${wasapi_function}" "${wasapi_original_argument}" wasapi_original_argument_count)
count_literal("${wasapi_function}" "${wasapi_patched_argument}" wasapi_patched_argument_count)
count_literal("${wasapi_function}" "client->Initialize (" wasapi_initialize_count)

if (NOT wasapi_declaration_count EQUAL 1
    OR NOT wasapi_original_argument_count EQUAL 1
    OR NOT wasapi_patched_argument_count EQUAL 0
    OR NOT wasapi_initialize_count EQUAL 1)
    message(FATAL_ERROR "Pinned JUCE WASAPI session tokens do not match the reviewed original state")
endif()

set(wasapi_expected_normalized "${wasapi_normalized}")
string(REPLACE "${wasapi_session_declaration}" ""
    wasapi_expected_normalized "${wasapi_expected_normalized}")
string(REPLACE "${wasapi_original_argument}" "${wasapi_patched_argument}"
    wasapi_expected_normalized "${wasapi_expected_normalized}")
string(SHA256 wasapi_expected_sha256 "${wasapi_expected_normalized}")
if (NOT wasapi_expected_sha256 STREQUAL wasapi_patched_sha256)
    message(FATAL_ERROR "Minimal WASAPI token transform did not produce the reviewed patched source")
endif()

# Apply the same two edits to the raw text so every unrelated byte and the
# checkout's existing newline style remain unchanged.
set(wasapi_updated_raw "${wasapi_raw}")
string(REPLACE "            GUID session;\r\n" ""
    wasapi_updated_raw "${wasapi_updated_raw}")
string(REPLACE "            GUID session;\n" ""
    wasapi_updated_raw "${wasapi_updated_raw}")
string(REPLACE "${wasapi_original_argument}" "${wasapi_patched_argument}"
    wasapi_updated_raw "${wasapi_updated_raw}")
string(REPLACE "\r\n" "\n" wasapi_updated_normalized "${wasapi_updated_raw}")
string(SHA256 wasapi_updated_sha256 "${wasapi_updated_normalized}")
if (NOT wasapi_updated_sha256 STREQUAL wasapi_patched_sha256)
    message(FATAL_ERROR "Raw WASAPI token transform failed verification; original source was not changed")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef wasapi_write_id)
set(wasapi_temporary "${wasapi_source}.micfxbridge-session-${wasapi_write_id}.tmp")
file(WRITE "${wasapi_temporary}" "${wasapi_updated_raw}")
file(READ "${wasapi_temporary}" wasapi_written_raw)
if (NOT wasapi_written_raw STREQUAL wasapi_updated_raw)
    file(REMOVE "${wasapi_temporary}")
    message(FATAL_ERROR "WASAPI patch temporary file failed byte verification")
endif()
file(RENAME "${wasapi_temporary}" "${wasapi_source}")
message(STATUS "Applied JUCE WASAPI default-session patch")
