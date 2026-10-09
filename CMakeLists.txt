
cmake_minimum_required(VERSION 3.25)

project(lord_of_rings
    VERSION 0.1.0
    LANGUAGES C
)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(lord_of_rings
    source/main.c
    source/uring/uring.c
)

find_program(CLANG_TIDY_EXE NAMES clang-tidy)

if(CLANG_TIDY_EXE)
    set_target_properties(lord_of_rings PROPERTIES
        C_CLANG_TIDY "${CLANG_TIDY_EXE}"
    )
endif()

target_include_directories(lord_of_rings
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/source"
)

target_compile_definitions(lord_of_rings PRIVATE _GNU_SOURCE)

target_compile_options(lord_of_rings PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wstrict-prototypes
)
