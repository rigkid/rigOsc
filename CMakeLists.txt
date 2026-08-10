file(READ "${CMAKE_CURRENT_LIST_DIR}/pack.json" PACK_JSON)
string(JSON PACK_NAME GET "${PACK_JSON}" name)

add_library(${PACK_NAME} STATIC
	src/rigOsc.cpp
	src/rigOsc.h
	src/UdpSocket.cpp
	src/UdpSocket.h
	src/OscCodec.cpp
	src/OscCodec.h
	src/CNetworkIdentity.h
	src/COscEndpoint.h
	src/COscShowBus.h
)
target_link_libraries(${PACK_NAME} PUBLIC rigkit)
if(WIN32)
	target_link_libraries(${PACK_NAME} PRIVATE ws2_32)
endif()
target_include_directories(${PACK_NAME} PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/src>
)
message(STATUS "Configured pack: ${PACK_NAME} (UDP OSC)")
