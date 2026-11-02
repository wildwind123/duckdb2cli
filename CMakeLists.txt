cmake_minimum_required(VERSION 3.16)

project(duckb2cli VERSION 0.1.0 DESCRIPTION "SQL client for DuckDB Quack servers" LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(DUCKB2CLI_WITH_READLINE "Use GNU readline for line editing and history (not on Windows)" ON)

# DuckDB C library. Point DUCKDB_ROOT at an unpacked duckdb-shared-libs-<os>-<arch> release archive
# (duckdb.h plus libduckdb.so / duckdb.lib + duckdb.dll) when it is not installed system-wide.
set(DUCKDB_ROOT "" CACHE PATH "Directory containing duckdb.h and the DuckDB shared library")
if(DUCKDB_ROOT)
  # An explicit location wins, also over a cross-compiling toolchain's sysroot.
  find_path(DUCKDB_INCLUDE_DIR duckdb.h PATHS "${DUCKDB_ROOT}" "${DUCKDB_ROOT}/include"
            NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  find_library(DUCKDB_LIBRARY NAMES duckdb libduckdb PATHS "${DUCKDB_ROOT}" "${DUCKDB_ROOT}/lib"
               NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif()
find_path(DUCKDB_INCLUDE_DIR duckdb.h)
find_library(DUCKDB_LIBRARY NAMES duckdb libduckdb)
if(NOT DUCKDB_INCLUDE_DIR OR NOT DUCKDB_LIBRARY)
  message(FATAL_ERROR "DuckDB not found. Download duckdb-shared-libs-<os>-<arch> from "
                      "https://github.com/duckdb/duckdb/releases and pass -DDUCKDB_ROOT=<dir>.")
endif()
message(STATUS "DuckDB headers: ${DUCKDB_INCLUDE_DIR}")
message(STATUS "DuckDB library: ${DUCKDB_LIBRARY}")

configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)

add_executable(duckb2cli
  src/main.c
  src/config.c
  src/format.c
  src/platform.c
  src/render.c
  src/shell.c
  src/util.c
)
target_include_directories(duckb2cli PRIVATE "${DUCKDB_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(duckb2cli PRIVATE "${DUCKDB_LIBRARY}")
# Build against the stable C API only: anything deprecated fails to compile.
target_compile_definitions(duckb2cli PRIVATE DUCKDB_API_NO_DEPRECATED)

if(MSVC)
  target_compile_options(duckb2cli PRIVATE /W4 /utf-8)
  target_compile_definitions(duckb2cli PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(duckb2cli PRIVATE -Wall -Wextra -Wshadow -Wstrict-prototypes)
endif()

if(WIN32)
  target_link_libraries(duckb2cli PRIVATE shell32)
  if(MINGW)
    # C99-conforming printf (%llu, PRIu64) in MinGW's runtime.
    target_compile_definitions(duckb2cli PRIVATE __USE_MINGW_ANSI_STDIO=1)
    target_link_options(duckb2cli PRIVATE -static-libgcc)
  endif()
  # duckdb.dll has to sit next to the executable.
  find_file(DUCKDB_DLL duckdb.dll HINTS "${DUCKDB_ROOT}" "${DUCKDB_ROOT}/bin" "${DUCKDB_ROOT}/lib"
            NO_CMAKE_FIND_ROOT_PATH)
  if(DUCKDB_DLL)
    add_custom_command(TARGET duckb2cli POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DUCKDB_DLL}" "$<TARGET_FILE_DIR:duckb2cli>")
    install(FILES "${DUCKDB_DLL}" DESTINATION bin)
  else()
    message(WARNING "duckdb.dll not found; copy it next to duckb2cli.exe before running.")
  endif()
else()
  if(DUCKB2CLI_WITH_READLINE)
    find_path(READLINE_INCLUDE_DIR readline/readline.h)
    find_library(READLINE_LIBRARY readline)
    if(READLINE_INCLUDE_DIR AND READLINE_LIBRARY)
      target_include_directories(duckb2cli PRIVATE "${READLINE_INCLUDE_DIR}")
      target_link_libraries(duckb2cli PRIVATE "${READLINE_LIBRARY}")
      target_compile_definitions(duckb2cli PRIVATE HAVE_READLINE)
      message(STATUS "readline: ${READLINE_LIBRARY}")
    else()
      message(STATUS "readline not found; building without line editing")
    endif()
  endif()
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
  target_link_libraries(duckb2cli PRIVATE Threads::Threads)
  target_compile_definitions(duckb2cli PRIVATE _POSIX_C_SOURCE=200809L)
  # Find libduckdb next to the binary or where it was found at build time.
  get_filename_component(DUCKDB_LIBRARY_DIR "${DUCKDB_LIBRARY}" DIRECTORY)
  set_target_properties(duckb2cli PROPERTIES
    BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
    INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib")
endif()

install(TARGETS duckb2cli RUNTIME DESTINATION bin)

enable_testing()
find_program(DUCKDB_CLI duckdb)
find_program(BASH_PROGRAM bash)
if(DUCKDB_CLI AND BASH_PROGRAM AND NOT WIN32)
  add_test(NAME smoke
           COMMAND "${BASH_PROGRAM}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_test.sh"
                   "$<TARGET_FILE:duckb2cli>" "${DUCKDB_CLI}")
endif()
