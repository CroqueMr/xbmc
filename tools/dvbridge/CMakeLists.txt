cmake_minimum_required(VERSION 3.22)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(DVBridgeCore LANGUAGES C)
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(DVPlacebo REQUIRED IMPORTED_TARGET libplacebo>=7.372.0)
pkg_check_modules(DVAvutil REQUIRED IMPORTED_TARGET libavutil>=59.12.100)
pkg_check_modules(DVAvcodec REQUIRED IMPORTED_TARGET libavcodec>=62)
pkg_check_modules(DVGles REQUIRED IMPORTED_TARGET glesv2)
pkg_check_modules(DVEgl REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(DVDrm REQUIRED IMPORTED_TARGET libdrm)
add_library(dvbridge_core STATIC dvbridge_core.c dvbridge_placebo.c dvbridge_render.c
                                dvbridge_gl_frame.c dvbridge_gl_pack.c dvbridge_fel.c dvbridge_dmabuf.c)
set_target_properties(dvbridge_core PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)
target_include_directories(dvbridge_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(dvbridge_core PUBLIC PkgConfig::DVPlacebo PkgConfig::DVAvutil
                                         PkgConfig::DVAvcodec PkgConfig::DVGles
                                         PkgConfig::DVEgl PkgConfig::DVDrm m)
