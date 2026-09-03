set(ICE_SRCS
    stun/stun_message.cpp
    stun/stun_auth.cpp
    session/ice_candidate.cpp
    session/ice_session.cpp
    turn/turn_client.cpp
    turn/turn_server.cpp
    agent/ice_agent.cpp
    sdp/ice_sdp.cpp
    mdns/mdns.cpp
)

set(ICE_HEADERS
    ice.h
    stun/stun_message.h
    stun/stun_auth.h
    session/ice_candidate.h
    session/candidate_pair.h
    session/ice_session.h
    session/ice_checklist.h
    turn/turn_client.h
    turn/turn_server.h
    agent/ice_agent.h
    agent/ice_config.h
    sdp/ice_sdp.h
    mdns/mdns.h
    onceToken.h
)

set(LIBS "hv_static")
if(WIN32 OR MINGW)
    list(APPEND LIBS iphlpapi)
endif()

if(WITH_OPENSSL)
    find_path(SRTP_INCLUDE_DIR NAMES srtp2/srtp.h)
    find_library(SRTP_LIBRARY NAMES srtp2 libsrtp2)
    if(SRTP_LIBRARY AND SRTP_INCLUDE_DIR)
        set(LIBS ${LIBS} ${SRTP_LIBRARY})
        aux_source_directory(rtc ICE_SRCS)
        aux_source_directory(rtp ICE_SRCS)
        list(APPEND ICE_SRCS sdp/Sdp.cpp Frame.cpp mini.cpp Stamp.cpp)
        list(APPEND ICE_HEADERS sdp/Sdp.h Frame.h mini.h Stamp.h config.h)
    else()
        message(FATAL_ERROR "WITH_OPENSSL=ON requires libsrtp headers and library (srtp2/srtp.h and srtp2/libsrtp2), but they were not found.")
    endif()
endif()

add_library(ice ${ICE_SRCS} ${ICE_HEADERS})
add_subdirectory(mp4)
list(APPEND LIBS mp4)
target_link_libraries(ice PUBLIC ${LIBS})

target_include_directories(ice PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/event
    ${CMAKE_SOURCE_DIR}/evpp
    ${CMAKE_SOURCE_DIR}/cpputil
    ${CMAKE_SOURCE_DIR}/base
    ${CMAKE_SOURCE_DIR}/ssl
    ${CMAKE_SOURCE_DIR}/util
    ${CMAKE_SOURCE_DIR}/http
    ${CMAKE_SOURCE_DIR}/http/server
)

if(WITH_OPENSSL)
    target_include_directories(ice PRIVATE ${SRTP_INCLUDE_DIR})
endif()

if(MSVC)
    target_compile_options(ice PRIVATE /utf-8)
endif()

add_subdirectory(examples)