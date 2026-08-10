#pragma once

#include <string>
#include <vector>
#include "ecs/PropertyReflection.h"

namespace rigkit {
namespace ecs {

/**
 * @brief Install / app network identity — data only.
 * @details Used in OSC address paths: /rigkit/<networkId>/…
 */
struct CNetworkIdentity {
	std::string networkId = "install";
	std::string bindAddress = "0.0.0.0"; ///< Listen bind (0.0.0.0 = all interfaces)
	std::string label = "RigKit";

	std::vector<sProp> GetProperties() {
		return {{0, "Network ID", EPT_STRING, &networkId},
				{1, "Bind Address", EPT_STRING, &bindAddress},
				{2, "Label", EPT_STRING, &label}};
	}
};

} // namespace ecs
} // namespace rigkit
