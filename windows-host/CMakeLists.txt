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

# Microsoft Edge WebView2 SDK — auto-downloaded during configure if not present.
# The static loader (WebView2LoaderStatic.lib) keeps the EXE self-contained.
set(WEBVIEW2_SDK_VERSION "1.0.2792.45")
set(WEBVIEW2_SDK_DIR "" CACHE PATH "Path to extracted Microsoft.Web.WebView2 NuGet package")
if(WEBVIEW2_SDK_DIR STREQUAL "")
  set(WEBVIEW2_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2")
endif()

if(NOT EXISTS "${WEBVIEW2_SDK_DIR}/build/native/include/WebView2.h")
  set(_wv2_nupkg "${CMAKE_CURRENT_BINARY_DIR}/webview2-sdk.zip")
  message(STATUS "WebView2 SDK not found — downloading v${WEBVIEW2_SDK_VERSION}...")
  file(DOWNLOAD
    "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${WEBVIEW2_SDK_VERSION}/microsoft.web.webview2.${WEBVIEW2_SDK_VERSION}.nupkg"
    "${_wv2_nupkg}"
    SHOW_PROGRESS
    STATUS _wv2_dl_status
  )
  list(GET _wv2_dl_status 0 _wv2_dl_code)
  if(NOT _wv2_dl_code EQUAL 0)
    message(FATAL_ERROR "WebView2 SDK download failed: ${_wv2_dl_status}")
  endif()
  file(ARCHIVE_EXTRACT INPUT "${_wv2_nupkg}" DESTINATION "${WEBVIEW2_SDK_DIR}")
  if(NOT EXISTS "${WEBVIEW2_SDK_DIR}/build/native/include/WebView2.h")
    message(FATAL_ERROR "WebView2 SDK extraction failed — expected: ${WEBVIEW2_SDK_DIR}/build/native/include/WebView2.h")
  endif()
  message(STATUS "WebView2 SDK ready at ${WEBVIEW2_SDK_DIR}")
endif()

# Download WebView2 Runtime bootstrapper for the installer if not already present
set(_wv2_bootstrapper "${CMAKE_CURRENT_SOURCE_DIR}/installer/MicrosoftEdgeWebview2Setup.exe")
if(NOT EXISTS "${_wv2_bootstrapper}")
  message(STATUS "Downloading WebView2 Runtime bootstrapper for installer...")
  file(DOWNLOAD
    "https://go.microsoft.com/fwlink/?LinkId=2124703"
    "${_wv2_bootstrapper}"
    SHOW_PROGRESS
    STATUS _wv2b_status
  )
  list(GET _wv2b_status 0 _wv2b_code)
  if(NOT _wv2b_code EQUAL 0)
    message(WARNING "WebView2 Runtime bootstrapper download failed (code ${_wv2b_code}) — installer will not bundle it.")
  endif()
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
  oleaut32    # required by WebView2LoaderStatic
  uuid        # required by WebView2LoaderStatic
  version     # required by WebView2LoaderStatic (GetFileVersionInfo*)
  shlwapi     # required by WebView2LoaderStatic (PathXxx helpers)
  advapi32    # required by WebView2LoaderStatic (ETW EventXxx + Registry RegXxx)
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
