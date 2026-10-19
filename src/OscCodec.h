#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace rigkit {
namespace osc {

/** @brief OSC NTP timetag. `seconds == 0 && fraction == 1` means immediate. */
struct OscTimetag {
	uint32_t seconds = 0;
	uint32_t fraction = 1;

	bool isImmediate() const { return seconds == 0 && fraction == 1; }
	uint64_t bits() const { return (uint64_t(seconds) << 32) | fraction; }

	static OscTimetag immediate() { return {}; }
	static OscTimetag fromBits(uint64_t ntp) {
		OscTimetag t;
		t.seconds = static_cast<uint32_t>(ntp >> 32);
		t.fraction = static_cast<uint32_t>(ntp);
		return t;
	}
};

/** @brief OSC 1.1 nil (`N`). */
struct OscNil {};
/** @brief OSC 1.1 infinitum (`I`). */
struct OscInf {};
/** @brief OSC 1.1 ASCII char (`c`), stored in the low byte of a 32-bit slot. */
struct OscChar {
	char value = 0;
};
/** @brief OSC 1.1 RGBA color (`r`). */
struct OscRgba {
	uint8_t r = 0;
	uint8_t g = 0;
	uint8_t b = 0;
	uint8_t a = 0;
};
/** @brief OSC 1.1 MIDI message (`m`): port, status, data1, data2. */
struct OscMidi {
	uint8_t port = 0;
	uint8_t status = 0;
	uint8_t data1 = 0;
	uint8_t data2 = 0;
};
/** @brief OSC 1.1 alternate string (`S`). */
struct OscSymbol {
	std::string value;
};

/**
 * @brief OSC 1.1 array (`[` `]`).
 * @details Nested arrays are values. Copying an array copies its items.
 * Read and write them through @ref items.
 */
struct OscArray {
	struct Store;
	std::shared_ptr<Store> store;

	OscArray();
	OscArray(const OscArray& other);
	OscArray(OscArray&& other) noexcept;
	OscArray& operator=(const OscArray& other);
	OscArray& operator=(OscArray&& other) noexcept;
	~OscArray();
};

using OscArg =
	std::variant<int32_t, float, std::string, bool, std::vector<uint8_t>, int64_t, double,
				 OscTimetag, OscNil, OscInf, OscChar, OscRgba, OscMidi, OscSymbol, OscArray>;

struct OscArray::Store {
	std::vector<OscArg> values;
};

inline OscArray::OscArray() : store(std::make_shared<Store>()) {}

inline OscArray::OscArray(const OscArray& other) : store(std::make_shared<Store>()) {
	if (other.store) {
		store->values = other.store->values;
	}
}

inline OscArray::OscArray(OscArray&& other) noexcept : store(std::move(other.store)) {
	if (!store) {
		store = std::make_shared<Store>();
	}
	if (!other.store) {
		other.store = std::make_shared<Store>();
	}
}

inline OscArray& OscArray::operator=(const OscArray& other) {
	if (this != &other) {
		store = std::make_shared<Store>();
		if (other.store) {
			store->values = other.store->values;
		}
	}
	return *this;
}

inline OscArray& OscArray::operator=(OscArray&& other) noexcept {
	if (this != &other) {
		store = std::move(other.store);
		if (!store) {
			store = std::make_shared<Store>();
		}
		if (!other.store) {
			other.store = std::make_shared<Store>();
		}
	}
	return *this;
}

inline OscArray::~OscArray() = default;

/** @brief Items of an OSC 1.1 array. */
inline std::vector<OscArg>& items(OscArray& array) {
	if (!array.store) {
		array.store = std::make_shared<OscArray::Store>();
	}
	return array.store->values;
}

/** @brief Items of an OSC 1.1 array. */
inline const std::vector<OscArg>& items(const OscArray& array) {
	if (!array.store) {
		static const std::vector<OscArg> kEmpty;
		return kEmpty;
	}
	return array.store->values;
}

struct OscMessage {
	std::string address;
	std::vector<OscArg> args;
	/**
	 * Set on parse from the bundle that held this message. Immediate for a bare
	 * message. `encodeMessage` does not write it; pass it to `encodeBundle`.
	 */
	OscTimetag time = OscTimetag::immediate();
};

/** @brief One received message plus UDP sender (empty host when unknown). */
struct OscRx {
	OscMessage msg;
	std::string fromHost;
	int fromPort = 0;
};

/** @brief Host clock as an OSC NTP timetag. */
OscTimetag timetagNow();

/** @brief Negative if `a` is earlier than `b`. */
inline int timetagCompare(OscTimetag a, OscTimetag b) {
	if (a.seconds != b.seconds) {
		return a.seconds < b.seconds ? -1 : 1;
	}
	if (a.fraction != b.fraction) {
		return a.fraction < b.fraction ? -1 : 1;
	}
	return 0;
}

/**
 * @brief When a nested bundle may run.
 * @details An immediate child waits for its parent. A later child waits for itself.
 */
inline OscTimetag effectiveTimetag(OscTimetag parent, OscTimetag bundle) {
	if (bundle.isImmediate()) {
		return parent;
	}
	if (parent.isImmediate()) {
		return bundle;
	}
	return timetagCompare(parent, bundle) >= 0 ? parent : bundle;
}

/**
 * @brief Parse one OSC packet into messages.
 * @details Bundles (including nested) expand in order. Each message `time` is
 * the effective timetag. Empty bundles succeed with `out` unchanged. An unknown
 * type tag fails that message. False only when the packet is not OSC.
 * `parent` is the enclosing bundle time; callers omit it.
 */
bool parsePacket(const uint8_t* data, size_t len, std::vector<OscMessage>& out,
				 OscTimetag parent = OscTimetag::immediate());

/** @brief Parse one packet; fills the first message. Bundles yield message 0. */
bool parsePacket(const uint8_t* data, size_t len, OscMessage& out);

/**
 * @brief Encode one OSC message.
 * @details Types: `i` `h` `f` `d` `s` `S` `b` `t` `c` `r` `m` `T` `F` `N` `I`,
 * and OSC 1.1 arrays `[` `]`.
 */
bool encodeMessage(const std::string& address, const std::vector<OscArg>& args,
				   std::vector<uint8_t>& out);

/** @brief Encode a bundle that runs at `time`. */
bool encodeBundle(const std::vector<OscMessage>& messages, OscTimetag time,
				  std::vector<uint8_t>& out);

/** @brief Encode a bundle with the immediate timetag. */
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
