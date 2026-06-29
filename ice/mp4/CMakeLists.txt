cmake_minimum_required(VERSION 3.14)

set(MP4_SOURCES
    Mp4Reader.cpp
    Mp4Reader.h
    Mp4Writer.cpp
    H264NalParse.cpp
    H264NalParse.h
    Mp4Writer.h
    SPSParser.c
    SPSParser.h
)
aux_source_directory(source MP4_SOURCES)
add_library(mp4 STATIC ${MP4_SOURCES})
target_link_libraries(mp4 PUBLIC hv_static)
target_include_directories(mp4 PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include 
    ${CMAKE_CURRENT_SOURCE_DIR}/.. 
    ${CMAKE_CURRENT_SOURCE_DIR}/../../base
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ssl
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cpputil
    ${CMAKE_CURRENT_SOURCE_DIR}/../../evpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../event)