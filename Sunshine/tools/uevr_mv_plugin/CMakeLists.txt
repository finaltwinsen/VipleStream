# ============================================================================
# CMakeLists.txt
# VipleStream §VR-GAME-MV (M-MV0) — 獨立 UEVR 動態向量擷取外掛建置設定
#
# 注意：
#   - 此 CMakeLists.txt 完全獨立，不納入 Sunshine 主專案 CMake。
#   - 透過同目錄下的 build.cmd 呼叫建置，產出 x64 Release DLL 至 repo 根目錄的
#     temp/uevr_mv_plugin/viple_uevr_mv_plugin.dll。
# ============================================================================

cmake_minimum_required(VERSION 3.20)
project(viple_uevr_mv_plugin VERSION 0.1.0 LANGUAGES CXX)

# third_party/uevr/API.hpp 使用了 C++20 consteval，因此必須啟用 C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(viple_uevr_mv_plugin SHARED
    plugin_main.cpp
    ngx_minimal.h
    cs_downsample_mv_8x8.hlsl
)

# 標記 .hlsl 為 HEADER_FILE_ONLY，避免 MSBuild/VS 內建 FXC 自動以錯誤的 entry point 編譯它
# （外掛於執行時透過 D3DCompileFromFile / D3DCompile 動態編譯 cs_downsample_mv_8x8）
set_source_files_properties(cs_downsample_mv_8x8.hlsl PROPERTIES HEADER_FILE_ONLY TRUE)

target_include_directories(viple_uevr_mv_plugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)

target_compile_definitions(viple_uevr_mv_plugin PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(viple_uevr_mv_plugin PRIVATE
        /utf-8
        /EHsc
        /W4
        /O2
        /Gy
        /Zc:inline
    )
endif()

target_link_libraries(viple_uevr_mv_plugin PRIVATE
    d3d12
    dxgi
    d3dcompiler
)

# 統一將輸出 DLL 放置在 CMAKE_BINARY_DIR (即 temp/uevr_mv_plugin/)
set_target_properties(viple_uevr_mv_plugin PROPERTIES
    OUTPUT_NAME "viple_uevr_mv_plugin"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
)

# 建置完成後將 cs_downsample_mv_8x8.hlsl 一併複製到輸出目錄，方便與 DLL 一起部署
add_custom_command(TARGET viple_uevr_mv_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/cs_downsample_mv_8x8.hlsl"
        "${CMAKE_BINARY_DIR}/cs_downsample_mv_8x8.hlsl"
    COMMENT "Copying cs_downsample_mv_8x8.hlsl to output directory"
)
