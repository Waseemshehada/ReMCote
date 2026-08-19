cmake_minimum_required(VERSION 3.24)
project(ReMCoteHost VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "ReMCoteHost targets Windows x64 only.")
endif()

# Dependencies via Conan 2 (see conanfile.txt):
#   libdatachannel/0.24.0 — WebRTC (peer connection, DTLS/SRTP, H264 packetization).
#     Media support is enabled in the ConanCenter package and RTC_ENABLE_MEDIA=1
#     is propagated by the package itself — do NOT define it manually here.
#   nlohmann_json — signaling / input message JSON
find_package(LibDataChannel CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# NVIDIA Video Codec SDK (NVENC). Headers only; nvEncodeAPI is loaded
# dynamically from the NVIDIA driver at runtime, so no SDK .lib is required.
set(NVCODEC_SDK_DIR "" CACHE PATH "Path to NVENC headers (dir containing ffnvcodec/nvEncodeAPI.h)")
if(NVCODEC_SDK_DIR STREQUAL "")
  set(NVCODEC_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nv-codec-headers/include")
endif()

# Microsoft Edge WebView2 SDK. The CI workflow downloads this pinned NuGet
# package before configuring CMake; the static loader keeps the EXE self-contained.
set(WEBVIEW2_SDK_DIR "" CACHE PATH "Path to extracted Microsoft.Web.WebView2 NuGet package")
if(WEBVIEW2_SDK_DIR STREQUAL "")
  set(WEBVIEW2_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2")
endif()
if(NOT EXISTS "${WEBVIEW2_SDK_DIR}/build/native/include/WebView2.h")
  message(FATAL_ERROR "WebView2 SDK not found. Set WEBVIEW2_SDK_DIR or run the Windows build workflow.")
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
  src/Logger.cpp
  src/DesktopViewer.cpp
)

target_include_directories(ReMCoteHost PRIVATE
  src
  ${NVCODEC_SDK_DIR}
  "${WEBVIEW2_SDK_DIR}/build/native/include"
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
  "${WEBVIEW2_SDK_DIR}/build/native/x64/WebView2LoaderStatic.lib"
)

target_compile_definitions(ReMCoteHost PRIVATE
  UNICODE _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  WEBVIEW2_STATIC
)

set_target_properties(ReMCoteHost PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
