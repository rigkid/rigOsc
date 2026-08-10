#include "OscCodec.h"
#include <cstring>

namespace rigkit {
namespace osc {
namespace {

size_t padded4(size_t n) {
	// Round up to a multiple of 4 (no-op when already aligned).
	return (n + 3) & ~size_t(3);
}

bool readString(const uint8_t* data, size_t len, size_t& i, std::string& out) {
	if (i >= len) {
		return false;
	}
	const size_t start = i;
	while (i < len && data[i] != 0) {
		++i;
	}
	if (i >= len) {
		return false;
	}
	out.assign(reinterpret_cast<const char*>(data + start), i - start);
	++i; // NUL
	i = padded4(i);
	return i <= len;
}

uint32_t readBe32(const uint8_t* p) {
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void writeBe32(std::vector<uint8_t>& out, uint32_t v) {
	out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
	out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
	out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
	out.push_back(static_cast<uint8_t>(v & 0xff));
}

void writePaddedString(std::vector<uint8_t>& out, const std::string& s) {
	out.insert(out.end(), s.begin(), s.end());
	out.push_back(0);
	while (out.size() & 3) {
		out.push_back(0);
	}
}

} // namespace

bool parsePacket(const uint8_t* data, size_t len, OscMessage& outMsg) {
	outMsg = {};
	if (!data || len < 4) {
		return false;
	}
	// Bundles: skip for v0.1
	if (data[0] == '#') {
		return false;
	}
	size_t i = 0;
	if (!readString(data, len, i, outMsg.address)) {
		return false;
	}
	std::string tags;
	if (!readString(data, len, i, tags) || tags.empty() || tags[0] != ',') {
		return false;
	}
	for (size_t t = 1; t < tags.size(); ++t) {
		const char tag = tags[t];
		if (tag == 'i') {
			if (i + 4 > len) {
				return false;
			}
			outMsg.args.push_back(static_cast<int32_t>(readBe32(data + i)));
			i += 4;
		} else if (tag == 'f') {
			if (i + 4 > len) {
				return false;
			}
			uint32_t bits = readBe32(data + i);
			float f = 0.f;
			std::memcpy(&f, &bits, 4);
			outMsg.args.push_back(f);
			i += 4;
		} else if (tag == 's') {
			std::string s;
			if (!readString(data, len, i, s)) {
				return false;
			}
			outMsg.args.push_back(std::move(s));
		} else if (tag == 'T') {
			outMsg.args.push_back(true);
		} else if (tag == 'F') {
			outMsg.args.push_back(false);
		} else {
			// Unsupported tag — stop cleanly
			break;
		}
	}
	return !outMsg.address.empty();
}

bool encodeMessage(const std::string& address, const std::vector<OscArg>& args,
				   std::vector<uint8_t>& out) {
	out.clear();
	if (address.empty() || address[0] != '/') {
		return false;
	}
	writePaddedString(out, address);
	std::string tags = ",";
	for (const auto& a : args) {
		if (std::holds_alternative<int32_t>(a)) {
			tags.push_back('i');
		} else if (std::holds_alternative<float>(a)) {
			tags.push_back('f');
		} else if (std::holds_alternative<std::string>(a)) {
			tags.push_back('s');
		} else if (std::holds_alternative<bool>(a)) {
			tags.push_back(std::get<bool>(a) ? 'T' : 'F');
		}
	}
	writePaddedString(out, tags);
	for (const auto& a : args) {
		if (const auto* vi = std::get_if<int32_t>(&a)) {
			writeBe32(out, static_cast<uint32_t>(*vi));
		} else if (const auto* vf = std::get_if<float>(&a)) {
			uint32_t bits = 0;
			std::memcpy(&bits, vf, 4);
			writeBe32(out, bits);
		} else if (const auto* vs = std::get_if<std::string>(&a)) {
			writePaddedString(out, *vs);
		}
		// T/F have no bytes
	}
	return true;
}

} // namespace osc
} // namespace rigkit
