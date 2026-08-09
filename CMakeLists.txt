cmake_minimum_required(VERSION 3.20)
project(proton_chess VERSION 0.2.0 LANGUAGES CXX)

option(PROTON_NATIVE "Tune release builds for the current CPU" OFF)
option(PROTON_LTO "Enable link-time optimisation when supported" ON)
option(PROTON_SANITIZERS "Enable address and undefined-behaviour sanitizers" OFF)
option(PROTON_BUILD_TESTS "Build engine correctness tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(proton_engine
    src/engine/eval.cpp
    src/engine/position.cpp
    src/engine/search.cpp
    src/engine/see.cpp
    src/engine/uci.cpp
)
target_include_directories(proton_engine PUBLIC src)
target_link_libraries(proton_engine PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(proton_engine PRIVATE /W4 /permissive- /EHsc)
    if(PROTON_NATIVE)
        target_compile_options(proton_engine PRIVATE /arch:AVX2)
    endif()
else()
    target_compile_options(proton_engine PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:RelWithDebInfo>:-O3>
    )
    if(PROTON_NATIVE)
        target_compile_options(proton_engine PRIVATE $<$<CONFIG:Release>:-march=native>)
    endif()
    if(PROTON_SANITIZERS)
        # PUBLIC is intentional: proton_engine is a static library, so the
        # sanitizer runtimes must also be linked by every final executable.
        target_compile_options(proton_engine PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(proton_engine PUBLIC -fsanitize=address,undefined)
    endif()
endif()

if(PROTON_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT PROTON_IPO_SUPPORTED OUTPUT PROTON_IPO_ERROR)
    if(PROTON_IPO_SUPPORTED)
        set_property(TARGET proton_engine PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET proton_engine PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    endif()
endif()

add_executable(proton_chess src/main.cpp)
target_link_libraries(proton_chess PRIVATE proton_engine)
if(PROTON_LTO AND PROTON_IPO_SUPPORTED)
    set_property(TARGET proton_chess PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET proton_chess PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()

if(PROTON_BUILD_TESTS)
    enable_testing()
    add_executable(proton-tests tests/test_engine.cpp)
    target_link_libraries(proton-tests PRIVATE proton_engine)
    add_test(NAME proton-tests COMMAND proton-tests)

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME proton-uci-smoke
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/uci_smoke.py
                         $<TARGET_FILE:proton_chess>)
        set_tests_properties(proton-uci-smoke PROPERTIES TIMEOUT 20)

        execute_process(
            COMMAND ${Python3_EXECUTABLE} -c "import chess"
            RESULT_VARIABLE PROTON_PYTHON_CHESS_AVAILABLE
            OUTPUT_QUIET ERROR_QUIET)
        if(PROTON_PYTHON_CHESS_AVAILABLE EQUAL 0)
            add_test(NAME proton-match-tool-tests
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_estimate_elo.py)
            set_tests_properties(proton-match-tool-tests PROPERTIES TIMEOUT 20)
        else()
            message(STATUS "python-chess not found; match-tool tests are disabled")
        endif()
    endif()
endif()
