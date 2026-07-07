# PowerGrade — cross-platform OpenFX plugin build (macOS / Windows / Linux).
# SPDX-License-Identifier: BSD-3-Clause
#
#   cmake -S . -B build-cmake [-DBUILD_CUDA=ON]
#   cmake --build build-cmake --config Release
#   ctest --test-dir build-cmake
#
# Output: build-cmake/PowerGrade.ofx.bundle
cmake_minimum_required(VERSION 3.20)
project(PowerGrade LANGUAGES CXX)

option(BUILD_CUDA "Compile the CUDA kernel and advertise CUDA render" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(SDK "${CMAKE_SOURCE_DIR}/third_party/openfx")

set(SUPPORT_SRCS
  ${SDK}/Support/Library/ofxsCore.cpp
  ${SDK}/Support/Library/ofxsImageEffect.cpp
  ${SDK}/Support/Library/ofxsInteract.cpp
  ${SDK}/Support/Library/ofxsLog.cpp
  ${SDK}/Support/Library/ofxsMultiThread.cpp
  ${SDK}/Support/Library/ofxsParams.cpp
  ${SDK}/Support/Library/ofxsProperty.cpp
  ${SDK}/Support/Library/ofxsPropertyValidation.cpp)

set(PLUGIN_SRCS src/PowerGrade.cpp src/OpenCLKernel.cpp)

find_package(OpenCL REQUIRED)

if(APPLE)
  list(APPEND PLUGIN_SRCS src/MetalKernel.mm)
  set(ARCHDIR "MacOS")
elseif(WIN32)
  set(ARCHDIR "Win64")
else()
  set(ARCHDIR "Linux-x86-64")
endif()

if(BUILD_CUDA)
  enable_language(CUDA)
  list(APPEND PLUGIN_SRCS src/CudaKernel.cu)
endif()

add_library(PowerGrade MODULE ${PLUGIN_SRCS} ${SUPPORT_SRCS})
target_include_directories(PowerGrade PRIVATE
  ${SDK}/include ${SDK}/Support/include ${SDK}/Support/Plugins/include src)
target_compile_definitions(PowerGrade PRIVATE OFX_SUPPORTS_OPENGLRENDER)
target_link_libraries(PowerGrade PRIVATE OpenCL::OpenCL)

set_target_properties(PowerGrade PROPERTIES PREFIX "" SUFFIX ".ofx" OUTPUT_NAME "PowerGrade")

if(APPLE)
  target_link_libraries(PowerGrade PRIVATE "-framework Metal" "-framework AppKit")
endif()
if(BUILD_CUDA)
  target_compile_definitions(PowerGrade PRIVATE OFX_SUPPORTS_CUDARENDER)
  set_target_properties(PowerGrade PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
endif()

# ---- assemble the .ofx.bundle next to the built module ----
set(BUNDLE "${CMAKE_BINARY_DIR}/PowerGrade.ofx.bundle")
add_custom_command(TARGET PowerGrade POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE}/Contents/${ARCHDIR}"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:PowerGrade>" "${BUNDLE}/Contents/${ARCHDIR}/PowerGrade.ofx"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/src/Info.plist" "${BUNDLE}/Contents/Info.plist"
  COMMENT "Assembling PowerGrade.ofx.bundle")

# ---- CPU unit tests ----
enable_testing()
add_executable(pipeline_test test/pipeline_test.cpp)
add_test(NAME pipeline COMMAND pipeline_test)
