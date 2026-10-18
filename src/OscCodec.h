#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace rigkit {
namespace osc {

using OscArg = std::variant<int32_t, float, std::string, bool, std::vector<uint8_t>>;

struct OscMessage {
	std::string address;
	std::vector<OscArg> args;
};

/** @brief One received message plus UDP sender (empty host when unknown). */
struct OscRx {
	OscMessage msg;
	std::string fromHost;
	int fromPort = 0;
};

/**
 * @brief Parse one OSC 1.0 packet into messages.
 * @details Bundles (including nested) expand in order. Empty bundles succeed
 * with `out` unchanged. Malformed elements are skipped; false only when the
 * packet is not OSC at all.
 */
bool parsePacket(const uint8_t* data, size_t len, std::vector<OscMessage>& out);

/** @brief Parse one packet; fills the first message. Bundles yield message 0. */
bool parsePacket(const uint8_t* data, size_t len, OscMessage& out);

/** @brief Encode a simple OSC 1.0 message (i / f / s / T / F / b). */
bool encodeMessage(const std::string& address, const std::vector<OscArg>& args,
				   std::vector<uint8_t>& out);

/** @brief Encode an immediate-timetag OSC 1.0 bundle of messages. */
bool encodeBundle(const std::vector<OscMessage>& messages, std::vector<uint8_t>& out);

/**
 * @brief OSC 1.0 address-pattern match (`?` `*` `[]` `{}`).
 * @details `*` does not cross `/`. `**` is a pack extension and matches across
 * `/`. A pattern ending in `/**` matches that node and every address under it.
 */
bool matchAddress(const std::string& pattern, const std::string& address);

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
