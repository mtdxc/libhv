cmake_minimum_required(VERSION 3.14)

add_executable(ice_test ice_test.cpp)
target_link_libraries(ice_test ice)
list(APPEND EXAMPLES ice_test)

add_executable(turn_server_test turn_server_test.cpp)
target_link_libraries(turn_server_test ice)
list(APPEND EXAMPLES turn_server)

if(WITH_OPENSSL)
    add_executable(rtc_test rtc_test.cpp)
    target_link_libraries(rtc_test ice)
    list(APPEND EXAMPLES rtc_test)

    add_executable(rtc_http_test rtc_http_test.cpp)
    target_link_libraries(rtc_http_test ice)
    list(APPEND EXAMPLES rtc_http_test)

    add_executable(rtc_whep_test rtc_whep_test.cpp)
    target_link_libraries(rtc_whep_test ice)
    list(APPEND EXAMPLES rtc_whep_test)

    add_executable(rtc_whip_test rtc_whip_test.cpp)
    target_link_libraries(rtc_whip_test ice)
    list(APPEND EXAMPLES rtc_whip_test)
endif()

add_subdirectory(sdl3)