set(ICE_SRCS
    stun/stun_message.cpp
    stun/stun_auth.cpp
    session/ice_candidate.cpp
    session/ice_session.cpp
    turn/turn_client.cpp
    turn/turn_server.cpp
    agent/ice_agent.cpp
    sdp/ice_sdp.cpp
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
)

add_library(ice ${ICE_SRCS} ${ICE_HEADERS})
set(LIBS "hv_static")
if(WIN32 OR MINGW)
    set(LIBS ${LIBS} iphlpapi)
endif()

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
)
