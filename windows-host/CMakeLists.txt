cmake_minimum_required(VERSION 3.24)
project(ReMCoteHost VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "ReMCoteHost targets Windows x64 only. Build with build-windows.ps1 on Windows.")
endif()

# Dependencies via vcpkg (manifest mode — see vcpkg.json):
#   libdatachannel — production WebRTC (peer connection, DTLS/SRTP, H264 packetization)
#   nlohmann-json  — signaling / input message JSON
find_package(LibDataChannel CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# NVIDIA Video Codec SDK (NVENC). Headers ship with the SDK; nvEncodeAPI is
# loaded dynamically from the NVIDIA driver at runtime, so no SDK .lib is
# required beyond the headers. Set NVCODEC_SDK_DIR or place the SDK under
# third_party/nv-codec-headers.
set(NVCODEC_SDK_DIR "" CACHE PATH "Path to NVIDIA Video Codec SDK (Interface dir with nvEncodeAPI.h)")
if(NVCODEC_SDK_DIR STREQUAL "")
  set(NVCODEC_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nv-codec-headers/include")
endif()

add_executable(ReMCoteHost WIN32
  src/main.cpp
  src/DeviceRegistration.cpp
  src/CaptureEngine.cpp
  src/EncoderEngine.cpp
  src/WebRtcTransport.cpp
  src/InputEngine.cpp
  src/PerformanceMonitor.cpp
  src/HostUI.cpp
)

target_include_directories(ReMCoteHost PRIVATE
  src
  ${NVCODEC_SDK_DIR}
)

target_link_libraries(ReMCoteHost PRIVATE
  LibDataChannel::LibDataChannel
  nlohmann_json::nlohmann_json
  d3d11
  dxgi
  ws2_32
  user32
  gdi32
  shell32
  ole32
)

target_compile_definitions(ReMCoteHost PRIVATE
  UNICODE _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  RTC_ENABLE_MEDIA=1
)

set_target_properties(ReMCoteHost PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
