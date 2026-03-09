cmake_minimum_required(VERSION 3.20)

project(ProtonChess VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PROTON_CHESS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

file(GLOB_RECURSE PROTON_CHESS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(proton_chess ${PROTON_CHESS_SOURCES})
target_include_directories(proton_chess PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(MSVC)
    target_compile_options(proton_chess PRIVATE /W4 /permissive-)
    if(PROTON_CHESS_WARNINGS_AS_ERRORS)
        target_compile_options(proton_chess PRIVATE /WX)
    endif()
else()
    target_compile_options(proton_chess PRIVATE -Wall -Wextra -Wpedantic)
    if(PROTON_CHESS_WARNINGS_AS_ERRORS)
        target_compile_options(proton_chess PRIVATE -Werror)
    endif()
endif()
