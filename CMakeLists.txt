cmake_minimum_required(VERSION 3.20)
project(SevenLegendsMouseFix LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "007 Legends Mouse Fix currently requires Microsoft Visual C++.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "007 Legends is 32-bit. Configure with -A Win32.")
endif()

option(BUILD_RESEARCH_TOOLS "Build smoke tests and reverse-engineering helpers" OFF)

add_library(dinput8 SHARED
    src/dinput8_proxy.cpp
    src/dinput8.def
)
target_compile_features(dinput8 PRIVATE cxx_std_17)
target_compile_definitions(dinput8 PRIVATE
    DIRECTINPUT_VERSION=0x0800
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(dinput8 PRIVATE /W4 /EHsc /GS)
target_link_options(dinput8 PRIVATE /DYNAMICBASE:NO /NXCOMPAT)
set_target_properties(dinput8 PROPERTIES
    OUTPUT_NAME "dinput8"
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
add_custom_command(TARGET dinput8 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/007MouseFix.ini"
            "$<TARGET_FILE_DIR:dinput8>/007MouseFix.ini"
)

if(BUILD_RESEARCH_TOOLS)
    add_executable(probe_smoke tests/probe_smoke.cpp)
    target_compile_features(probe_smoke PRIVATE cxx_std_17)
    target_compile_definitions(probe_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(probe_smoke PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    add_dependencies(probe_smoke dinput8)

    add_executable(watch_smoke tests/watch_smoke.cpp)
    target_compile_features(watch_smoke PRIVATE cxx_std_17)
    target_compile_definitions(watch_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(watch_smoke PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )

    add_executable(read_process_memory tools/read_process_memory.cpp)
    target_compile_features(read_process_memory PRIVATE cxx_std_17)
    target_compile_definitions(read_process_memory PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(read_process_memory PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
