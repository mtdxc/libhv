set(ICE_SRCS
    stun/stun_message.cpp
    stun/stun_auth.cpp
    candidate/ice_candidate.cpp
    session/ice_session.cpp
    turn/turn_client.cpp
    agent/ice_agent.cpp
    sdp/ice_sdp.cpp
)

set(ICE_HEADERS
    ice.h
    stun/stun_message.h
    stun/stun_auth.h
    candidate/ice_candidate.h
    candidate/candidate_pair.h
    session/ice_session.h
    session/ice_checklist.h
    turn/turn_client.h
    agent/ice_agent.h
    agent/ice_config.h
    sdp/ice_sdp.h
)

# Prepend directory path
list(TRANSFORM ICE_SRCS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM ICE_HEADERS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

# Export to parent scope
set(ICE_SRCS ${ICE_SRCS} PARENT_SCOPE)
set(ICE_HEADERS ${ICE_HEADERS} PARENT_SCOPE)
