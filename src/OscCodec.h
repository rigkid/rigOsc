#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace rigkit {
namespace osc {

using OscArg = std::variant<int32_t, float, std::string, bool>;

struct OscMessage {
	std::string address;
	std::vector<OscArg> args;
};

/** @brief Parse one OSC packet (message only; bundles ignored for v0.1). */
bool parsePacket(const uint8_t* data, size_t len, OscMessage& out);

/** @brief Encode a simple OSC message. */
bool encodeMessage(const std::string& address, const std::vector<OscArg>& args,
				   std::vector<uint8_t>& out);

inline bool encodeFloat(const std::string& address, float v, std::vector<uint8_t>& out) {
	return encodeMessage(address, {v}, out);
}
inline bool encodeInt(const std::string& address, int32_t v, std::vector<uint8_t>& out) {
	return encodeMessage(address, {v}, out);
}
inline bool encodeString(const std::string& address, const std::string& v,
						 std::vector<uint8_t>& out) {
	return encodeMessage(address, {v}, out);
}

} // namespace osc
} // namespace rigkit
