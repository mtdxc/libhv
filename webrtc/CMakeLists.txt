
function(set_file_group prefix)
    message(STATUS "set_file_group " ${prefix} " " ${ARGC})
    foreach(FILE IN LISTS ARGN 1)
       # Get the directory of the source file
       get_filename_component(PARENT_DIR "${FILE}" DIRECTORY)

       # Remove common directory prefix to make the group
       string(REPLACE "${prefix}" "" GROUP "${PARENT_DIR}")

      # Make sure we are using windows slashes
       string(REPLACE "/" "\\" GROUP "${GROUP}")

       # Group into "Source Files" and "Header Files"
       #if ("${FILE}" MATCHES ".*\\.cpp")
       #   set(GROUP "Source Files${GROUP}")
       #elseif("${FILE}" MATCHES ".*\\.h")
       #   set(GROUP "Header Files${GROUP}")
       #endif()
       #message(STATUS ${GROUP} " : " ${FILE})
       source_group("${GROUP}" FILES "${FILE}")
    endforeach()
endfunction()

include_directories(. .. ../base ../ssl ../event ../util ../evpp ../cpputil ../http)
set(RTC_Root ${CMAKE_CURRENT_SOURCE_DIR})
#file(GLOB RTC_SRCS ${RTC_Root}/*.cpp ${RTC_Root}/*.hpp ${RTC_Root}/*.h ${RTC_Root}/*/*.cpp ${RTC_Root}/*/*.c ${RTC_Root}/*/*.hpp ${RTC_Root}/*/*.h)
#set_file_group(${RTC_Root} ${RTC_SRCS})
aux_source_directory(. RTC_SRCS)
aux_source_directory(Util RTC_SRCS)
aux_source_directory(Common RTC_SRCS)
aux_source_directory(Extension RTC_SRCS)
aux_source_directory(Rtcp RTC_SRCS)
aux_source_directory(Player RTC_SRCS)
aux_source_directory(Pusher RTC_SRCS)
aux_source_directory(Rtmp RTC_SRCS)
aux_source_directory(Rtsp RTC_SRCS)
aux_source_directory(webrtc RTC_SRCS)

add_executable(rtc_server ${RTC_SRCS})
target_link_libraries(rtc_server srtp2 ${HV_LIBRARIES})
