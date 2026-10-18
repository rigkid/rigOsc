#include "OscCodec.h"

#include <cstring>

namespace rigkit {
namespace osc {
namespace {

size_t padded4(size_t n) {
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

bool parseOneMessage(const uint8_t* data, size_t len, OscMessage& outMsg) {
	outMsg = {};
	if (!data || len < 4 || data[0] == '#') {
		return false;
	}
	size_t i = 0;
	if (!readString(data, len, i, outMsg.address) || outMsg.address.empty() ||
		outMsg.address[0] != '/') {
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
		} else if (tag == 'b') {
			if (i + 4 > len) {
				return false;
			}
			const uint32_t sz = readBe32(data + i);
			i += 4;
			if (i + sz > len) {
				return false;
			}
			std::vector<uint8_t> blob(data + i, data + i + sz);
			outMsg.args.push_back(std::move(blob));
			i += padded4(sz);
			if (i > len) {
				return false;
			}
		} else if (tag == 'T') {
			outMsg.args.push_back(true);
		} else if (tag == 'F') {
			outMsg.args.push_back(false);
		} else {
			// Unsupported tag — keep what we have, stop cleanly.
			break;
		}
	}
	return true;
}

bool isBundle(const uint8_t* data, size_t len) {
	return data && len >= 8 && std::memcmp(data, "#bundle", 7) == 0 && data[7] == 0;
}

bool matchHere(const char* p, const char* a);

bool matchClass(const char*& p, char c) {
	if (*p != '[') {
		return false;
	}
	++p;
	bool neg = false;
	if (*p == '!') {
		neg = true;
		++p;
	}
	bool matched = false;
	if (*p == ']') {
		if (c == ']') {
			matched = true;
		}
		++p;
	}
	while (*p && *p != ']') {
		if (p[1] == '-' && p[2] && p[2] != ']') {
			const char lo = p[0];
			const char hi = p[2];
			if (c >= lo && c <= hi) {
				matched = true;
			}
			p += 3;
		} else {
			if (*p == c) {
				matched = true;
			}
			++p;
		}
	}
	if (*p == ']') {
		++p;
	}
	return neg ? !matched : matched;
}

bool matchBrace(const char* p, const char* a) {
	if (*p != '{') {
		return false;
	}
	++p;
	const char* close = std::strchr(p, '}');
	if (!close) {
		return false;
	}
	const char* rest = close + 1;
	const char* start = p;
	for (const char* q = p; q <= close; ++q) {
		if (q == close || *q == ',') {
			std::string alt(start, q);
			alt.append(rest);
			if (matchHere(alt.c_str(), a)) {
				return true;
			}
			start = q + 1;
		}
	}
	return false;
}

bool restEmptyOk(const char* p) {
	while (*p == '/') {
		++p;
	}
	while (*p == '*') {
		if (p[1] == '*') {
			p += 2;
			while (*p == '/') {
				++p;
			}
		} else {
			++p;
		}
	}
	return !*p;
}

bool matchHere(const char* p, const char* a) {
	while (*p && *a) {
		if (p[0] == '*' && p[1] == '*') {
			p += 2;
			if (*p == '/') {
				++p;
			}
			if (!*p) {
				return true;
			}
			while (true) {
				if (matchHere(p, a)) {
					return true;
				}
				if (!*a) {
					return false;
				}
				++a;
			}
		}
		if (*p == '*') {
			++p;
			while (true) {
				if (matchHere(p, a)) {
					return true;
				}
				if (!*a || *a == '/') {
					return false;
				}
				++a;
			}
		}
		if (*p == '?') {
			if (*a == '/') {
				return false;
			}
			++p;
			++a;
			continue;
		}
		if (*p == '[') {
			if (*a == '/') {
				return false;
			}
			if (!matchClass(p, *a)) {
				return false;
			}
			++a;
			continue;
		}
		if (*p == '{') {
			return matchBrace(p, a);
		}
		if (*p != *a) {
			return false;
		}
		++p;
		++a;
	}
	if (*a) {
		return false;
	}
	return restEmptyOk(p);
}

} // namespace

bool parsePacket(const uint8_t* data, size_t len, std::vector<OscMessage>& out) {
	if (!data || len < 4) {
		return false;
	}
	if (isBundle(data, len)) {
		if (len < 16) {
			return false;
		}
		size_t i = 16;
		while (i + 4 <= len) {
			const uint32_t sz = readBe32(data + i);
			i += 4;
			if (sz == 0 || (sz & 3) != 0 || i + sz > len) {
				break;
			}
			parsePacket(data + i, sz, out);
			i += sz;
		}
		return true;
	}
	OscMessage msg;
	if (!parseOneMessage(data, len, msg)) {
		return false;
	}
	out.push_back(std::move(msg));
	return true;
}

bool parsePacket(const uint8_t* data, size_t len, OscMessage& out) {
	std::vector<OscMessage> msgs;
	if (!parsePacket(data, len, msgs) || msgs.empty()) {
		out = {};
		return false;
	}
	out = std::move(msgs.front());
	return true;
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
		} else if (std::holds_alternative<std::vector<uint8_t>>(a)) {
			tags.push_back('b');
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
		} else if (const auto* vb = std::get_if<std::vector<uint8_t>>(&a)) {
			writeBe32(out, static_cast<uint32_t>(vb->size()));
			out.insert(out.end(), vb->begin(), vb->end());
			while (out.size() & 3) {
				out.push_back(0);
			}
		}
	}
	return true;
}

bool encodeBundle(const std::vector<OscMessage>& messages, std::vector<uint8_t>& out) {
	out.clear();
	writePaddedString(out, "#bundle");
	// Immediate timetag (1).
	for (int i = 0; i < 7; ++i) {
		out.push_back(0);
	}
	out.push_back(1);
	for (const OscMessage& m : messages) {
		std::vector<uint8_t> elem;
		if (!encodeMessage(m.address, m.args, elem) || (elem.size() & 3) != 0) {
			out.clear();
			return false;
		}
		writeBe32(out, static_cast<uint32_t>(elem.size()));
		out.insert(out.end(), elem.begin(), elem.end());
	}
	return true;
}

bool matchAddress(const std::string& pattern, const std::string& address) {
	if (pattern.empty() || address.empty() || pattern[0] != '/' || address[0] != '/') {
		return false;
	}
	return matchHere(pattern.c_str(), address.c_str());
}

} // namespace osc
} // namespace rigkit
