add_executable(audio_server server.cpp shine_mp3.c)
target_link_libraries(audio_server ${HV_LIBRARIES})