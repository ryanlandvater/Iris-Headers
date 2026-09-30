# CMake for Iris Header Installation
# 2025 Copyright Ryan Landvater
# Created 3/3/2025
cmake_minimum_required(VERSION 3.11...3.30)

# Iris requires C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(IRIS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/)
set(IRIS_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/)
Project (IrisHeadersProj LANGUAGES CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(IrisHeaders INTERFACE)
target_include_directories(
    IrisHeaders INTERFACE
    $<BUILD_INTERFACE:${IRIS_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_sources(
    IrisHeaders INTERFACE FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include/
    FILES 
    ${IRIS_INCLUDE_DIR}/IrisTypes.hpp
    ${IRIS_INCLUDE_DIR}/IrisCore.hpp
    ${IRIS_INCLUDE_DIR}/IrisCodecTypes.hpp
    ${IRIS_INCLUDE_DIR}/IrisCodecCore.hpp
)
if(NOT ${PROJECT_IS_TOP_LEVEL})
    
endif(NOT ${PROJECT_IS_TOP_LEVEL})

option(IRIS_HEADERS_BUILD_TESTS "Build the Iris-Headers source tests (requires Google Highway)" OFF)
if (IRIS_HEADERS_BUILD_TESTS)
    enable_testing()
    find_package(hwy REQUIRED)
    add_executable(
        IrisSIMDTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/IrisSIMDTests.cpp
        ${IRIS_SOURCE_DIR}/IrisSIMD.cpp
        ${IRIS_SOURCE_DIR}/IrisBuffer.cpp
    )
    target_include_directories(IrisSIMDTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/priv)
    target_compile_definitions(IrisSIMDTests PRIVATE IRIS_EXPORT_API=true)
    target_link_libraries(IrisSIMDTests PRIVATE IrisHeaders hwy::hwy)
    add_test(NAME IrisSIMDTests COMMAND IrisSIMDTests)
endif()

if (${PROJECT_IS_TOP_LEVEL})
    install(
        TARGETS IrisHeaders
        EXPORT IrisHeadersConfig
        FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Iris
    )
    install(
        EXPORT IrisHeadersConfig
        FILE IrisHeadersConfig.cmake
        NAMESPACE "Iris::"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Iris
    )
endif()
