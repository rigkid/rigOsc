#pragma once

#include <string>
#include <vector>
#include "ecs/PropertyReflection.h"

namespace rigkit {
namespace ecs {

/**
 * @brief Show-control bus shared between OSC transport and the install app.
 * @details Incoming OSC sets fields + dirty flags. App applies dirty → local state.
 * App writes heartbeat/status/color; pack may emit them on send.
 */
struct COscShowBus {
	float master = 1.f;
	bool blackout = false;
	float colorR = 1.f;
	float colorG = 0.72f;
	float colorB = 0.35f;
	std::string status = "idle";
	int heartbeat = 0;
	/// Network id of the last inbound message that carried a sender string arg.
	std::string lastFrom;

	bool masterFromNet = false;
	bool blackoutFromNet = false;
	bool colorFromNet = false;
	bool statusFromNet = false;

	std::vector<sProp> GetProperties() {
		return {{0, "Master", EPT_FLOAT, &master},
				{1, "Blackout", EPT_BOOL, &blackout},
				{2, "Color R", EPT_FLOAT, &colorR},
				{3, "Color G", EPT_FLOAT, &colorG},
				{4, "Color B", EPT_FLOAT, &colorB},
				{5, "Status", EPT_STRING, &status},
				{6, "Heartbeat", EPT_INT, &heartbeat},
				{7, "Last From", EPT_STRING, &lastFrom}};
	}
};

} // namespace ecs
} // namespace rigkit
