cmake_minimum_required(VERSION 3.24)
project(MultiPaste VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(multipaste_core STATIC
    src/core.cpp
)
target_include_directories(multipaste_core PUBLIC include)
target_compile_definitions(multipaste_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(MultiPaste WIN32
    src/main.cpp
)
target_link_libraries(MultiPaste PRIVATE
    multipaste_core
    comctl32
    ole32
    shell32
    windowsapp
    advapi32
)
target_compile_definitions(MultiPaste PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

enable_testing()
add_executable(multipaste_core_tests tests/core_tests.cpp)
target_link_libraries(multipaste_core_tests PRIVATE multipaste_core)
add_test(NAME multipaste_core_tests COMMAND multipaste_core_tests)
add_test(
    NAME release_config_tests
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/release_config_tests.ps1
)

if(MSVC)
    target_compile_options(multipaste_core PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(MultiPaste PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(multipaste_core_tests PRIVATE /W4 /permissive- /utf-8)
endif()
