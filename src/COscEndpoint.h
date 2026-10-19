#pragma once

#include <string>
#include <vector>
#include "ecs/PropertyReflection.h"

namespace rigkit {
namespace ecs {

/** @brief `COscEndpoint::streamFraming` labels. 0 = size prefix, 1 = SLIP. */
inline const char* const kOscStreamFramingNames[] = {"Size", "Slip"};

/**
 * @brief OSC listen / send endpoints - data only.
 * @details Speaks `rig.io.osc`. Socket handles live in rigOsc transport code.
 * `addressPrefix` is the show-bus app segment (`/oscHost`). Empty at setup
 * is filled from the host app name.
 * TCP and multicast stay off until set. `tcpPort` 0 uses `listenPort`.
 * `multicastGroup` empty keeps the exclusive unicast UDP bind.
 */
struct COscEndpoint {
	bool listenEnabled = true;
	int listenPort = 8000;
	bool sendEnabled = true;
	std::string sendHost = "127.0.0.1";
	int sendPort = 8001;
	std::string addressPrefix; ///< `/<appName>`. Empty until setup copies the host app name.
	bool tcpListen = false;
	int tcpPort = 0;			///< 0 uses `listenPort`.
	bool tcpConnect = false;	///< Dial `sendHost:sendPort` and send there on TCP.
	int streamFraming = 0;		///< 0 size prefix, 1 SLIP. See @ref kOscStreamFramingNames.
	std::string multicastGroup; ///< IPv4 group. Empty = unicast.
	int multicastTtl = 1;

	std::vector<sProp> GetProperties() {
		return {{0, "Listen", EPT_BOOL, &listenEnabled},
				{1, "Listen Port", EPT_INT, &listenPort},
				{2, "Send", EPT_BOOL, &sendEnabled},
				{3, "Send Host", EPT_STRING, &sendHost},
				{4, "Send Port", EPT_INT, &sendPort},
				{5, "Address Prefix", EPT_STRING, &addressPrefix},
				{6, "TCP Listen", EPT_BOOL, &tcpListen},
				{7, "TCP Port", EPT_INT, &tcpPort},
				{8, "TCP Connect", EPT_BOOL, &tcpConnect},
				{9, "Stream Framing", EPT_ENUM, &streamFraming, kOscStreamFramingNames, 2},
				{10, "Multicast Group", EPT_STRING, &multicastGroup},
				{11, "Multicast TTL", EPT_INT, &multicastTtl}};
	}
};

} // namespace ecs
} // namespace rigkit
