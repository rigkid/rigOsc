#pragma once

#include <string>
#include <vector>
#include "ecs/PropertyReflection.h"

namespace rigkit {
namespace ecs {

/**
 * @brief OSC UDP listen / send endpoints - data only.
 * @details Speaks `rig.io.osc`. Socket handles live in rigOsc transport code.
 * `addressPrefix` is the show-bus app segment (`/oscHost`). Empty at setup
 * is filled from the host app name.
 */
struct COscEndpoint {
	bool listenEnabled = true;
	int listenPort = 8000;
	bool sendEnabled = true;
	std::string sendHost = "127.0.0.1";
	int sendPort = 8001;
	std::string addressPrefix; ///< `/<appName>`. Empty until setup copies the host app name.

	std::vector<sProp> GetProperties() {
		return {{0, "Listen", EPT_BOOL, &listenEnabled},
				{1, "Listen Port", EPT_INT, &listenPort},
				{2, "Send", EPT_BOOL, &sendEnabled},
				{3, "Send Host", EPT_STRING, &sendHost},
				{4, "Send Port", EPT_INT, &sendPort},
				{5, "Address Prefix", EPT_STRING, &addressPrefix}};
	}
};

} // namespace ecs
} // namespace rigkit
