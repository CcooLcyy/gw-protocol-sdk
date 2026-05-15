cmake_minimum_required(VERSION 3.20)

project(gw_protocol_sdk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_library(gw_protocol_core STATIC
    src/core/session.cpp
)
target_include_directories(gw_protocol_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
set_target_properties(gw_protocol_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

function(add_protocol_library target source)
    add_library(${target} SHARED ${source})
    target_link_libraries(${target} PRIVATE gw_protocol_core)
    target_include_directories(${target}
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(${target} PRIVATE GW_PROTOCOL_SDK_BUILD)
    set_target_properties(${target} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
    )
endfunction()

add_protocol_library(GW_m101 src/exports/m101_api.cpp)
add_protocol_library(GW_iec101 src/exports/iec101_api.cpp)
add_protocol_library(GW_iec104 src/exports/iec104_api.cpp)

if(BUILD_TESTING)
    add_executable(gw_protocol_tests
        tests/test_main.cpp
        tests/test_lifecycle.cpp
    )
    target_include_directories(gw_protocol_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(gw_protocol_tests PRIVATE
        GW_m101
        GW_iec101
        GW_iec104
    )
    set(gw_protocol_test_cases
        m101.validate_config
        m101.lifecycle
        m101.raw_asdu
        m101.general_interrogation
        m101.counter_interrogation
        m101.read_point
        iec101.validate_config
        iec101.lifecycle
        iec101.raw_asdu
        iec101.general_interrogation
        iec101.counter_interrogation
        iec101.read_point
        iec104.validate_config
        iec104.lifecycle
        iec104.raw_asdu
        iec104.general_interrogation
        iec104.counter_interrogation
        iec104.read_point
    )
    foreach(test_case IN LISTS gw_protocol_test_cases)
        add_test(NAME ${test_case} COMMAND gw_protocol_tests ${test_case})
    endforeach()
endif()

add_executable(check_api_exports
    tools/check_api_exports.cpp
)
target_link_libraries(check_api_exports PRIVATE ${CMAKE_DL_LIBS})

add_custom_target(check_release_api
    COMMAND check_api_exports
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/api_exports.txt
        $<TARGET_FILE_DIR:GW_m101>
    DEPENDS
        check_api_exports
        GW_m101
        GW_iec101
        GW_iec104
    COMMENT "Checking release API exports against cmake/api_exports.txt"
)
