# MSVC's structured output is authoritative. Never derive dependencies from
# localized console output, whose decoder has dropped individual include lines.
if (MSVC AND CMAKE_GENERATOR STREQUAL "NMake Makefiles")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.27)
        message(FATAL_ERROR "MSVC 19.27 or newer is required for /sourceDependencies")
    endif()
    set(CMAKE_DEPFILE_FLAGS_CXX "/sourceDependencies <DEP_FILE>.json")
    set(CMAKE_CXX_DEPFILE_FORMAT gcc)
    set(CMAKE_CXX_DEPENDS_USE_COMPILER TRUE)
    list(PREPEND CMAKE_CXX_COMPILE_OBJECT
        "\"${CMAKE_COMMAND}\" -E rm -f <DEP_FILE>.json <DEP_FILE> <OBJECT>")
    list(APPEND CMAKE_CXX_COMPILE_OBJECT
        "\"${CMAKE_COMMAND}\" \"-DINPUT=<DEP_FILE>.json\" \"-DOUTPUT=<DEP_FILE>\" \"-DOBJECT=<OBJECT>\" -P \"${CMAKE_CURRENT_LIST_DIR}/WriteMsvcDepfile.cmake\"")
endif()
