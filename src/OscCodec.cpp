#include "OscCodec.h"

#include <chrono>
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

uint64_t readBe64(const uint8_t* p) {
	return (uint64_t(readBe32(p)) << 32) | readBe32(p + 4);
}

void writeBe64(std::vector<uint8_t>& out, uint64_t v) {
	writeBe32(out, static_cast<uint32_t>(v >> 32));
	writeBe32(out, static_cast<uint32_t>(v));
}

bool read4(const uint8_t* data, size_t len, size_t& i, uint32_t& out) {
	if (i + 4 > len) {
		return false;
	}
	out = readBe32(data + i);
	i += 4;
	return true;
}

bool read8(const uint8_t* data, size_t len, size_t& i, uint64_t& out) {
	if (i + 8 > len) {
		return false;
	}
	out = readBe64(data + i);
	i += 8;
	return true;
}

void writePaddedString(std::vector<uint8_t>& out, const std::string& s) {
	out.insert(out.end(), s.begin(), s.end());
	out.push_back(0);
	while (out.size() & 3) {
		out.push_back(0);
	}
}

bool parseOneTag(char tag, const uint8_t* data, size_t len, size_t& i, OscArg& out) {
	if (tag == 'i') {
		uint32_t u = 0;
		if (!read4(data, len, i, u)) {
			return false;
		}
		out = static_cast<int32_t>(u);
	} else if (tag == 'h' || tag == 't') {
		uint64_t u = 0;
		if (!read8(data, len, i, u)) {
			return false;
		}
		if (tag == 'h') {
			out = static_cast<int64_t>(u);
		} else {
			out = OscTimetag::fromBits(u);
		}
	} else if (tag == 'f') {
		uint32_t bits = 0;
		if (!read4(data, len, i, bits)) {
			return false;
		}
		float f = 0.f;
		std::memcpy(&f, &bits, 4);
		out = f;
	} else if (tag == 'd') {
		uint64_t bits = 0;
		if (!read8(data, len, i, bits)) {
			return false;
		}
		double d = 0.0;
		std::memcpy(&d, &bits, 8);
		out = d;
	} else if (tag == 's' || tag == 'S') {
		std::string s;
		if (!readString(data, len, i, s)) {
			return false;
		}
		if (tag == 's') {
			out = std::move(s);
		} else {
			out = OscSymbol{std::move(s)};
		}
	} else if (tag == 'b') {
		uint32_t sz = 0;
		if (!read4(data, len, i, sz) || i + sz > len) {
			return false;
		}
		std::vector<uint8_t> blob(data + i, data + i + sz);
		out = std::move(blob);
		i += padded4(sz);
		if (i > len) {
			return false;
		}
	} else if (tag == 'c') {
		uint32_t u = 0;
		if (!read4(data, len, i, u)) {
			return false;
		}
		out = OscChar{static_cast<char>(u & 0xff)};
	} else if (tag == 'r') {
		if (i + 4 > len) {
			return false;
		}
		out = OscRgba{data[i], data[i + 1], data[i + 2], data[i + 3]};
		i += 4;
	} else if (tag == 'm') {
		if (i + 4 > len) {
			return false;
		}
		out = OscMidi{data[i], data[i + 1], data[i + 2], data[i + 3]};
		i += 4;
	} else if (tag == 'T') {
		out = true;
	} else if (tag == 'F') {
		out = false;
	} else if (tag == 'N') {
		out = OscNil{};
	} else if (tag == 'I') {
		out = OscInf{};
	} else {
		return false;
	}
	return true;
}

bool parseArgs(const std::string& tags, size_t& t, const uint8_t* data, size_t len, size_t& i,
			   std::vector<OscArg>& out, bool inArray) {
	while (t < tags.size()) {
		const char tag = tags[t];
		if (tag == ']') {
			return inArray;
		}
		if (tag == '[') {
			++t;
			OscArray nested;
			if (!parseArgs(tags, t, data, len, i, items(nested), true)) {
				return false;
			}
			if (t >= tags.size() || tags[t] != ']') {
				return false;
			}
			++t;
			out.push_back(std::move(nested));
			continue;
		}
		OscArg arg;
		if (!parseOneTag(tag, data, len, i, arg)) {
			return false;
		}
		out.push_back(std::move(arg));
		++t;
	}
	return !inArray;
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
	size_t t = 1;
	return parseArgs(tags, t, data, len, i, outMsg.args, false);
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

bool appendArg(const OscArg& a, std::string& tags, std::vector<uint8_t>& body) {
	if (const auto* vi = std::get_if<int32_t>(&a)) {
		tags.push_back('i');
		writeBe32(body, static_cast<uint32_t>(*vi));
	} else if (const auto* vh = std::get_if<int64_t>(&a)) {
		tags.push_back('h');
		writeBe64(body, static_cast<uint64_t>(*vh));
	} else if (const auto* vf = std::get_if<float>(&a)) {
		tags.push_back('f');
		uint32_t bits = 0;
		std::memcpy(&bits, vf, 4);
		writeBe32(body, bits);
	} else if (const auto* vd = std::get_if<double>(&a)) {
		tags.push_back('d');
		uint64_t bits = 0;
		std::memcpy(&bits, vd, 8);
		writeBe64(body, bits);
	} else if (const auto* vs = std::get_if<std::string>(&a)) {
		tags.push_back('s');
		writePaddedString(body, *vs);
	} else if (const auto* sym = std::get_if<OscSymbol>(&a)) {
		tags.push_back('S');
		writePaddedString(body, sym->value);
	} else if (const auto* vb = std::get_if<std::vector<uint8_t>>(&a)) {
		tags.push_back('b');
		writeBe32(body, static_cast<uint32_t>(vb->size()));
		body.insert(body.end(), vb->begin(), vb->end());
		while (body.size() & 3) {
			body.push_back(0);
		}
	} else if (const auto* vt = std::get_if<OscTimetag>(&a)) {
		tags.push_back('t');
		writeBe64(body, vt->bits());
	} else if (const auto* vc = std::get_if<OscChar>(&a)) {
		tags.push_back('c');
		writeBe32(body, static_cast<uint32_t>(static_cast<uint8_t>(vc->value)));
	} else if (const auto* vr = std::get_if<OscRgba>(&a)) {
		tags.push_back('r');
		body.push_back(vr->r);
		body.push_back(vr->g);
		body.push_back(vr->b);
		body.push_back(vr->a);
	} else if (const auto* vm = std::get_if<OscMidi>(&a)) {
		tags.push_back('m');
		body.push_back(vm->port);
		body.push_back(vm->status);
		body.push_back(vm->data1);
		body.push_back(vm->data2);
	} else if (std::holds_alternative<bool>(a)) {
		tags.push_back(std::get<bool>(a) ? 'T' : 'F');
	} else if (std::holds_alternative<OscNil>(a)) {
		tags.push_back('N');
	} else if (std::holds_alternative<OscInf>(a)) {
		tags.push_back('I');
	} else if (const auto* arr = std::get_if<OscArray>(&a)) {
		tags.push_back('[');
		for (const OscArg& inner : items(*arr)) {
			if (!appendArg(inner, tags, body)) {
				return false;
			}
		}
		tags.push_back(']');
	} else {
		return false;
	}
	return true;
}

} // namespace

OscTimetag timetagNow() {
	using namespace std::chrono;
	const auto us = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
	const uint64_t sec = static_cast<uint64_t>(us / 1000000) + 2208988800ull;
	const uint64_t frac = static_cast<uint64_t>(us % 1000000) * (1ull << 32) / 1000000ull;
	return OscTimetag::fromBits((sec << 32) | frac);
}

bool parsePacket(const uint8_t* data, size_t len, std::vector<OscMessage>& out, OscTimetag parent) {
	if (!data || len < 4) {
		return false;
	}
	if (isBundle(data, len)) {
		if (len < 16) {
			return false;
		}
		const OscTimetag bundleTime = OscTimetag::fromBits(readBe64(data + 8));
		const OscTimetag when = effectiveTimetag(parent, bundleTime);
		size_t i = 16;
		while (i + 4 <= len) {
			const uint32_t sz = readBe32(data + i);
			i += 4;
			if (sz == 0 || (sz & 3) != 0 || i + sz > len) {
				break;
			}
			parsePacket(data + i, sz, out, when);
			i += sz;
		}
		return true;
	}
	OscMessage msg;
	if (!parseOneMessage(data, len, msg)) {
		return false;
	}
	msg.time = parent;
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
	std::string tags = ",";
	std::vector<uint8_t> body;
	for (const OscArg& a : args) {
		if (!appendArg(a, tags, body)) {
			return false;
		}
	}
	writePaddedString(out, address);
	writePaddedString(out, tags);
	out.insert(out.end(), body.begin(), body.end());
	return true;
}

bool encodeBundle(const std::vector<OscMessage>& messages, OscTimetag time,
				  std::vector<uint8_t>& out) {
	out.clear();
	writePaddedString(out, "#bundle");
	writeBe64(out, time.bits());
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

bool encodeBundle(const std::vector<OscMessage>& messages, std::vector<uint8_t>& out) {
	return encodeBundle(messages, OscTimetag::immediate(), out);
}

bool matchAddress(const std::string& pattern, const std::string& address) {
	if (pattern.empty() || address.empty() || pattern[0] != '/' || address[0] != '/') {
		return false;
	}
	return matchHere(pattern.c_str(), address.c_str());
}

} // namespace osc
} // namespace rigkit
