cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(BRASSLUA_IS_TOP_LEVEL ON)
else()
    set(BRASSLUA_IS_TOP_LEVEL OFF)
endif()

if(BRASSLUA_IS_TOP_LEVEL AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(_brasslua_vcpkg_root "$ENV{VCPKG_ROOT}")
    else()
        foreach(_candidate "D:/vcpkg" "$ENV{HOME}/vcpkg" "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg")
            if(EXISTS "${_candidate}/scripts/buildsystems/vcpkg.cmake")
                set(_brasslua_vcpkg_root "${_candidate}")
                message(STATUS "vcpkg: auto-detected at ${_brasslua_vcpkg_root}")
                break()
            endif()
        endforeach()
    endif()
    if(DEFINED _brasslua_vcpkg_root)
        set(CMAKE_TOOLCHAIN_FILE "${_brasslua_vcpkg_root}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
    endif()
endif()

project(brasslua VERSION 0.1.0 LANGUAGES C CXX)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/bro_deps.cmake)
option(BRASSLUA_ENABLE_BRONZE "Build brasslua_bronze bridge target" ${BRASSLUA_IS_TOP_LEVEL})

if(NOT TARGET brass)
    bro_dependency(brass GITHUB wlejon/brass)
endif()
if(BRASSLUA_ENABLE_BRONZE AND NOT TARGET bronze_runtime_shared)
    bro_dependency(bronze GITHUB wlejon/bronze TARGET bronze_runtime_shared
                   OPTIONS BRONZE_BUILD_SHARED_RUNTIME=ON BRONZE_BUILD_TESTS=OFF)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(BRASSLUA_IS_TOP_LEVEL)
    # Bound Ninja compile/link concurrency per AGENTS.md Section 5 (16 GB RAM envelope)
    set(BRO_COMPILE_JOBS "4" CACHE STRING "Max concurrent compile jobs")
    set(BRO_LINK_JOBS "2" CACHE STRING "Max concurrent link jobs")
    set_property(GLOBAL PROPERTY JOB_POOLS
        compile_job_pool=${BRO_COMPILE_JOBS}
        link_job_pool=${BRO_LINK_JOBS}
    )
    set(CMAKE_JOB_POOL_COMPILE compile_job_pool)
    set(CMAKE_JOB_POOL_LINK link_job_pool)
endif()

find_package(Threads REQUIRED)

add_library(brasslua STATIC
    src/lex/lexer.cpp
    src/parse/parser.cpp
    src/sema/symbol_resolve.cpp
    src/sema/type_infer.cpp
    src/runtime/runtime_state.cpp
    src/runtime/runtime_abi.cpp
    src/runtime/runtime_abi_ops.cpp
    src/runtime/stdlib_core.cpp
    src/runtime/stdlib_math_bit.cpp
    src/runtime/stdlib_string_table.cpp
    src/codegen/mir_lowering.cpp
    src/codegen/mir_lowering_table.cpp
    src/codegen/mir_lowering_expr.cpp
    src/codegen/mir_lowering_stmt.cpp
    src/codegen/jit_runner.cpp
)
add_library(brasslua::brasslua ALIAS brasslua)
if(NOT MSVC)
    target_compile_options(brasslua PRIVATE -O3)
endif()

# A Lua error is a C++ exception, and it is thrown through the runtime's
# extern "C" entry points (brasslua_rt_call, the table and arithmetic slow
# paths) on its way from `error` to the `pcall` that catches it. MSVC's default
# /EHsc assumes an extern "C" function never throws, so it drops the handlers
# around such calls and the error ends the process instead; /EHc- keeps them.
# Every target that catches a Lua error compiles with it.
function(brasslua_extern_c_may_throw target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /EHc-)
    endif()
endfunction()
brasslua_extern_c_may_throw(brasslua)

target_include_directories(brasslua PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(DEFINED brass_SOURCE_DIR AND EXISTS "${brass_SOURCE_DIR}/include")
    target_include_directories(brasslua PUBLIC "${brass_SOURCE_DIR}/include")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../brass/include")
    target_include_directories(brasslua PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../brass/include")
endif()

set(BRASSLUA_PLATFORM_LIBS)
if(NOT MSVC)
    set(BRASSLUA_PLATFORM_LIBS ${CMAKE_DL_LIBS} m)
endif()

target_link_libraries(brasslua PUBLIC
    brass
    Threads::Threads
    ${BRASSLUA_PLATFORM_LIBS}
)

add_executable(brasslua_cli src/cli/main.cpp)
set_target_properties(brasslua_cli PROPERTIES OUTPUT_NAME brasslua)
target_link_libraries(brasslua_cli PRIVATE brasslua)
brasslua_extern_c_may_throw(brasslua_cli)

if(BRASSLUA_ENABLE_BRONZE AND TARGET bronze_runtime_shared)
    add_library(brasslua_bronze STATIC
        src/bronze/bronze_bridge.cpp
    )
    add_library(brasslua::bronze ALIAS brasslua_bronze)
    if(NOT MSVC)
        target_compile_options(brasslua_bronze PRIVATE -O3)
    endif()
    brasslua_extern_c_may_throw(brasslua_bronze)
    target_include_directories(brasslua_bronze PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(brasslua_bronze PUBLIC
        brasslua
        bronze_runtime_shared
    )
endif()

option(BRASSLUA_BUILD_TESTS "Build brasslua tests" ${BRASSLUA_IS_TOP_LEVEL})
if(BRASSLUA_BUILD_TESTS)
    enable_testing()
    add_executable(brasslua_tests
        tests/test_main.cpp
        tests/test_lexer_parser.cpp
        tests/test_runtime_jit.cpp
        tests/test_luajit_oracle.cpp
    )
    target_link_libraries(brasslua_tests PRIVATE brasslua)
    brasslua_extern_c_may_throw(brasslua_tests)
    add_test(NAME brasslua_unit_and_oracle_tests COMMAND brasslua_tests)

    if(TARGET brasslua_bronze)
        add_executable(brasslua_bronze_tests
            tests/test_bronze_bridge.cpp
        )
        target_link_libraries(brasslua_bronze_tests PRIVATE brasslua_bronze)
        brasslua_extern_c_may_throw(brasslua_bronze_tests)
        add_test(NAME brasslua_bronze_bridge_tests COMMAND brasslua_bronze_tests)
    endif()
endif()
