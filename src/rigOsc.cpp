#include "rigOsc.h"

#include "CNetworkIdentity.h"
#include "COscEndpoint.h"
#include "UdpSocket.h"
#include "core/RigKitEngine.h"
#include "core/pack/PackRegistry.h"

#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>

namespace rigkit {
namespace {

bool oscChar(unsigned char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
		   c == '-' || c == '.';
}

std::string oscSegment(const std::string& raw) {
	std::string out;
	out.reserve(raw.size());
	for (unsigned char c : raw) {
		if (oscChar(c)) {
			out.push_back(static_cast<char>(c));
		}
	}
	return out.empty() ? "app" : out;
}

} // namespace

rigOsc::rigOsc() : IPack("rigOsc") {
	m_sock = std::make_unique<osc::UdpSocket>();
}

rigOsc::~rigOsc() {
	cleanup();
}

bool rigOsc::init() {
	spdlog::info("[rigOsc] init");
	return true;
}

void rigOsc::ensureEntity() {
	auto* engine = getEngine();
	if (!engine || !engine->getECSManager()) {
		return;
	}
	auto* ecs = engine->getECSManager();
	if (m_entity != entt::null && ecs->registry().valid(m_entity)) {
		return;
	}
	if (auto existing = ecs->findEntity("rigosc"); existing != entt::null) {
		m_entity = existing;
	} else {
		m_entity = ecs->createEntity("rigosc");
	}
	if (!ecs->hasComponent<ecs::CNetworkIdentity>(m_entity)) {
		ecs->addComponent<ecs::CNetworkIdentity>(m_entity, {});
	}
	if (!ecs->hasComponent<ecs::COscEndpoint>(m_entity)) {
		ecs->addComponent<ecs::COscEndpoint>(m_entity, {});
	}
}

void rigOsc::setup() {
	auto* engine = getEngine();
	if (!engine || !engine->getECSManager()) {
		return;
	}
	auto* ecs = engine->getECSManager();
	ecs->registerComponent<ecs::CNetworkIdentity>("NetworkIdentity", true);
	ecs->registerComponent<ecs::COscEndpoint>("OscEndpoint", true);
	ensureEntity();
	if (endpoint().addressPrefix.empty()) {
		endpoint().addressPrefix = busPrefix();
	}
	applyEndpoint();
	spdlog::info("[rigOsc] setup - id='{}' bus {} listen {}:{}", identity().networkId,
				 endpoint().addressPrefix, identity().bindAddress, endpoint().listenPort);
}

void rigOsc::cleanup() {
	if (m_sock) {
		m_sock->close();
	}
	m_boundPort = -1;
	m_boundAddress.clear();
	m_bindFailed = false;
	m_bindFailPort = -1;
	m_bindFailAddress.clear();
}

ecs::CNetworkIdentity& rigOsc::identity() {
	ensureEntity();
	return getEngine()->getECSManager()->getComponent<ecs::CNetworkIdentity>(m_entity);
}

ecs::COscEndpoint& rigOsc::endpoint() {
	ensureEntity();
	return getEngine()->getECSManager()->getComponent<ecs::COscEndpoint>(m_entity);
}

bool rigOsc::listening() const {
	return m_sock && m_sock->isOpen();
}

bool rigOsc::applyEndpoint(bool force) {
	ensureEntity();
	auto& ep = endpoint();
	auto& id = identity();
	if (!ep.listenEnabled) {
		m_bindFailed = false;
		m_lastError.clear();
		// Drop a listen bind; keep an ephemeral send socket if present.
		if (m_sock->isOpen() && m_boundPort == ep.listenPort) {
			m_sock->close();
			m_boundPort = -1;
			m_boundAddress.clear();
		}
		return true;
	}
	if (m_sock->isOpen() && m_boundPort == ep.listenPort && m_boundAddress == id.bindAddress) {
		return true;
	}
	if (!force && m_bindFailed && m_bindFailPort == ep.listenPort &&
		m_bindFailAddress == id.bindAddress) {
		return false;
	}
	m_lastError.clear();
	if (!m_sock->bind(id.bindAddress, ep.listenPort)) {
		m_lastError = m_sock->lastError();
		m_bindFailed = true;
		m_bindFailPort = ep.listenPort;
		m_bindFailAddress = id.bindAddress;
		spdlog::error("[rigOsc] bind {}:{} failed - {}", id.bindAddress, ep.listenPort,
					  m_lastError);
		return false;
	}
	m_bindFailed = false;
	m_bindFailPort = -1;
	m_bindFailAddress.clear();
	// Port 0 to kernel picks ephemeral; record the real bound port.
	m_boundPort = m_sock->port();
	if (ep.listenPort == 0) {
		ep.listenPort = m_boundPort;
	}
	m_boundAddress = id.bindAddress;
	spdlog::info("[rigOsc] listening on {}:{}", id.bindAddress, m_boundPort);
	return true;
}

const char* rigOsc::commandLineHelp() {
	return "  --network-id=ID / --id=ID   OSC network id (default: install)\n"
		   "  --listen-port=N             UDP listen port (default: 8000)\n"
		   "  --send-host=HOST            UDP send host (default: 127.0.0.1)\n"
		   "  --send-port=N               UDP send port (default: 8001)\n"
		   "  --bind-address=ADDR         Listen bind (default: 0.0.0.0)";
}

bool rigOsc::applyCommandLine(const CommandLineArgs& args) {
	ensureEntity();
	auto& id = identity();
	auto& ep = endpoint();

	if (auto v = args.getValue("network-id")) {
		id.networkId = *v;
	} else if (auto v = args.getValue("id")) {
		id.networkId = *v;
	}
	if (auto v = args.getValue("bind-address")) {
		id.bindAddress = *v;
	}
	if (auto v = args.getValue("listen-port")) {
		ep.listenPort = std::stoi(*v);
	}
	if (auto v = args.getValue("send-host")) {
		ep.sendHost = *v;
	}
	if (auto v = args.getValue("send-port")) {
		ep.sendPort = std::stoi(*v);
	}

	const bool ok = applyEndpoint();
	spdlog::info("[rigOsc] id='{}' listen {}:{} send {}:{} listening={}", id.networkId,
				 id.bindAddress, ep.listenPort, ep.sendHost, ep.sendPort,
				 listening() ? "yes" : "no");
	return ok;
}

std::string rigOsc::busPrefix() const {
	auto* self = const_cast<rigOsc*>(this);
	std::string raw = self->endpoint().addressPrefix;
	if (raw.empty()) {
		if (auto* engine = getEngine()) {
			if (const auto* app = engine->getApp()) {
				raw = app->getAppName();
			}
		}
	}
	while (!raw.empty() && raw.front() == '/') {
		raw.erase(raw.begin());
	}
	const auto slash = raw.find('/');
	if (slash != std::string::npos) {
		raw.resize(slash);
	}
	return "/" + oscSegment(raw);
}

std::string rigOsc::makeAddress(const std::string& leaf) const {
	auto* self = const_cast<rigOsc*>(this);
	return busPrefix() + "/" + oscSegment(self->identity().networkId) + "/" + oscSegment(leaf);
}

std::string rigOsc::makeBroadcastAddress(const std::string& leaf) const {
	return busPrefix() + "/" + oscSegment(leaf);
}

std::string rigOsc::busSenderId(const std::string& address) const {
	const std::string prefix = busPrefix() + "/";
	if (address.size() <= prefix.size() || address.compare(0, prefix.size(), prefix) != 0) {
		return {};
	}
	const std::string rest = address.substr(prefix.size());
	const auto slash = rest.find('/');
	if (slash == std::string::npos || slash == 0) {
		return {};
	}
	if (rest.find('/', slash + 1) != std::string::npos) {
		return {};
	}
	return rest.substr(0, slash);
}

bool rigOsc::busLeafMatches(const std::string& leaf, const std::string& address) const {
	const std::string seg = oscSegment(leaf);
	if (address == busPrefix() + "/" + seg) {
		return true;
	}
	const std::string prefix = busPrefix() + "/";
	const std::string tail = "/" + seg;
	if (address.size() <= prefix.size() + tail.size()) {
		return false;
	}
	if (address.compare(0, prefix.size(), prefix) != 0) {
		return false;
	}
	if (address.compare(address.size() - tail.size(), tail.size(), tail) != 0) {
		return false;
	}
	const std::string mid =
		address.substr(prefix.size(), address.size() - prefix.size() - tail.size());
	return !mid.empty() && mid.find('/') == std::string::npos;
}

bool rigOsc::send(const std::string& address, const std::vector<osc::OscArg>& args) {
	ensureEntity();
	auto& ep = endpoint();
	if (!ep.sendEnabled) {
		return false;
	}
	return encodeAndSend(ep.sendHost, ep.sendPort, address, args);
}

int rigOsc::addListener(const std::string& pattern, OscListener fn) {
	if (pattern.empty() || !fn) {
		return 0;
	}
	Listener row;
	row.id = m_nextListenerId++;
	row.pattern = pattern;
	row.fn = std::move(fn);
	const int id = row.id;
	m_listeners.push_back(std::move(row));
	return id;
}

int rigOsc::addBusListener(const std::string& leaf, OscListener fn) {
	if (leaf.empty() || !fn) {
		return 0;
	}
	Listener row;
	row.id = m_nextListenerId++;
	row.leaf = oscSegment(leaf);
	row.fn = std::move(fn);
	const int id = row.id;
	m_listeners.push_back(std::move(row));
	return id;
}

void rigOsc::removeListener(int id) {
	if (id <= 0) {
		return;
	}
	m_listeners.erase(std::remove_if(m_listeners.begin(), m_listeners.end(),
									 [id](const Listener& row) { return row.id == id; }),
					  m_listeners.end());
}

void rigOsc::dispatch(const osc::OscRx& rx) {
	for (Listener& row : m_listeners) {
		if (!row.fn) {
			continue;
		}
		const bool hit = !row.leaf.empty() ? busLeafMatches(row.leaf, rx.msg.address)
										   : osc::matchAddress(row.pattern, rx.msg.address);
		if (hit) {
			row.fn(rx);
		}
	}
}

bool rigOsc::encodeAndSend(const std::string& host, int port, const std::string& address,
						   const std::vector<osc::OscArg>& args) {
	if (!m_sock->isOpen()) {
		if (!m_sock->bind(identity().bindAddress, 0)) {
			m_lastError = m_sock->lastError();
			return false;
		}
		m_boundPort = m_sock->port();
		m_boundAddress = identity().bindAddress;
	}
	if (!osc::encodeMessage(address, args, m_txBuf)) {
		m_lastError = "encode failed";
		return false;
	}
	if (!m_sock->sendTo(host, port, m_txBuf.data(), m_txBuf.size())) {
		m_lastError = m_sock->lastError();
		return false;
	}
	return true;
}

bool rigOsc::sendTo(const std::string& host, int port, const std::string& address,
					const std::vector<osc::OscArg>& args) {
	ensureEntity();
	return encodeAndSend(host, port, address, args);
}

void rigOsc::update(float deltaTime) {
	(void)deltaTime;
	ensureEntity();
	applyEndpoint();

	if (m_sock->isOpen()) {
		for (int i = 0; i < 32; ++i) {
			std::string fromHost;
			int fromPort = 0;
			m_rxBuf.clear();
			if (m_sock->recvFrom(m_rxBuf, &fromHost, &fromPort) == 0) {
				break;
			}
			std::vector<osc::OscMessage> msgs;
			if (!osc::parsePacket(m_rxBuf.data(), m_rxBuf.size(), msgs) || msgs.empty()) {
				++m_ignoredPackets;
				continue;
			}
			for (osc::OscMessage& msg : msgs) {
				osc::OscRx rx;
				rx.msg = std::move(msg);
				rx.fromHost = fromHost;
				rx.fromPort = fromPort;
				m_lastRx = rx;
				m_haveRx = true;
				dispatch(rx);
			}
		}
	}
}

bool rigOsc::smokeLoopback() {
	ensureEntity();
	auto& ep = endpoint();
	auto& id = identity();

	// Snapshot caller endpoint - smoke must not steal a peer's listen port.
	const bool savedListenEn = ep.listenEnabled;
	const bool savedSendEn = ep.sendEnabled;
	const int savedListenPort = ep.listenPort;
	const int savedSendPort = ep.sendPort;
	const std::string savedSendHost = ep.sendHost;
	const std::string savedBind = id.bindAddress;

	auto restore = [&]() {
		ep.listenEnabled = savedListenEn;
		ep.sendEnabled = savedSendEn;
		ep.listenPort = savedListenPort;
		ep.sendPort = savedSendPort;
		ep.sendHost = savedSendHost;
		id.bindAddress = savedBind;
		applyEndpoint();
	};

	ep.listenEnabled = true;
	ep.sendEnabled = true;
	ep.sendHost = "127.0.0.1";
	id.bindAddress = "127.0.0.1";
	ep.listenPort = 0; // ephemeral - never collide with another instance on 8000
	if (!applyEndpoint()) {
		restore();
		return false;
	}
	const int port = m_boundPort;
	ep.sendPort = port;

	if (ep.addressPrefix.empty()) {
		ep.addressPrefix = busPrefix();
	}
	const std::string directed = makeAddress("ping");
	const std::string broadcast = makeBroadcastAddress("ping");
	const std::string idSeg = oscSegment(id.networkId);
	if (directed != busPrefix() + "/" + idSeg + "/ping" || broadcast != busPrefix() + "/ping" ||
		busSenderId(directed) != idSeg || !busSenderId(broadcast).empty()) {
		m_lastError = "smokeLoopback: bus address shape";
		spdlog::error("[rigOsc] {}", m_lastError);
		restore();
		return false;
	}

	float got = -1.f;
	std::string sawFrom;
	int hits = 0;
	const int busId = addBusListener("ping", [&](const osc::OscRx& rx) {
		if (rx.msg.args.empty()) {
			return;
		}
		if (const auto* f = std::get_if<float>(&rx.msg.args[0])) {
			got = *f;
			sawFrom = busSenderId(rx.msg.address);
			++hits;
		}
	});
	if (busId <= 0) {
		m_lastError = "smokeLoopback: addBusListener failed";
		spdlog::error("[rigOsc] {}", m_lastError);
		restore();
		return false;
	}

	auto waitGot = [&](float expect, const std::string& from) -> bool {
		for (int i = 0; i < 20; ++i) {
			update(0.f);
			if (hits > 0 && got == expect && sawFrom == from) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return false;
	};

	std::vector<uint8_t> pkt;
	if (!osc::encodeMessage(directed, {0.42f}, pkt)) {
		m_lastError = "encode smoke failed";
		removeListener(busId);
		restore();
		return false;
	}
	if (!m_sock->sendTo("127.0.0.1", port, pkt.data(), pkt.size())) {
		m_lastError = m_sock->lastError().empty() ? "sendTo failed" : m_sock->lastError();
		removeListener(busId);
		restore();
		return false;
	}
	if (!waitGot(0.42f, idSeg)) {
		m_lastError = "smokeLoopback: no packet received (directed)";
		spdlog::error("[rigOsc] {}", m_lastError);
		removeListener(busId);
		restore();
		return false;
	}

	hits = 0;
	got = -1.f;
	sawFrom.clear();
	pkt.clear();
	if (!osc::encodeMessage(broadcast, {0.55f}, pkt)) {
		m_lastError = "encode broadcast smoke failed";
		removeListener(busId);
		restore();
		return false;
	}
	if (!m_sock->sendTo("127.0.0.1", port, pkt.data(), pkt.size())) {
		m_lastError = m_sock->lastError().empty() ? "sendTo failed" : m_sock->lastError();
		removeListener(busId);
		restore();
		return false;
	}
	if (!waitGot(0.55f, {})) {
		m_lastError = "smokeLoopback: no packet received (broadcast)";
		spdlog::error("[rigOsc] {}", m_lastError);
		removeListener(busId);
		restore();
		return false;
	}

	hits = 0;
	got = -1.f;
	sawFrom.clear();
	pkt.clear();
	const std::string other = busPrefix() + "/other/ping";
	if (!osc::encodeMessage(other, {0.77f}, pkt) ||
		!m_sock->sendTo("127.0.0.1", port, pkt.data(), pkt.size()) || !waitGot(0.77f, "other")) {
		m_lastError = "smokeLoopback: other network id not delivered";
		spdlog::error("[rigOsc] {}", m_lastError);
		removeListener(busId);
		restore();
		return false;
	}
	removeListener(busId);

	auto failCodec = [&](const char* why) {
		m_lastError = why;
		spdlog::error("[rigOsc] {}", m_lastError);
		restore();
		return false;
	};
	if (!osc::matchAddress("/bus/mix/*", "/bus/mix/level") ||
		osc::matchAddress("/bus/mix/*", "/bus/mix/a/b") ||
		!osc::matchAddress("/bus/**", "/bus/mix/level") || !osc::matchAddress("/bus/**", "/bus") ||
		!osc::matchAddress("/bus/ch/?/set", "/bus/ch/a/set") ||
		!osc::matchAddress("/foo[abc]", "/foob") || !osc::matchAddress("/foo[!abc]", "/food") ||
		!osc::matchAddress("/{a,b}/x", "/a/x") || osc::matchAddress("/{a,b}/x", "/c/x")) {
		return failCodec("smokeLoopback: matchAddress failed");
	}

	{
		const std::vector<uint8_t> blob{1, 2, 3, 4, 5};
		pkt.clear();
		if (!osc::encodeMessage("/blob", {blob}, pkt)) {
			return failCodec("smokeLoopback: encode blob failed");
		}
		osc::OscMessage got;
		if (!osc::parsePacket(pkt.data(), pkt.size(), got)) {
			return failCodec("smokeLoopback: parse blob failed");
		}
		const auto* gotBlob = std::get_if<std::vector<uint8_t>>(&got.args[0]);
		if (got.address != "/blob" || !gotBlob || *gotBlob != blob) {
			return failCodec("smokeLoopback: blob roundtrip failed");
		}
	}

	{
		osc::OscMessage a;
		a.address = "/bundle/a";
		a.args.push_back(1);
		osc::OscMessage b;
		b.address = "/bundle/b";
		b.args.push_back(2.f);
		pkt.clear();
		if (!osc::encodeBundle({a, b}, pkt)) {
			return failCodec("smokeLoopback: encode bundle failed");
		}
		std::vector<osc::OscMessage> got;
		if (!osc::parsePacket(pkt.data(), pkt.size(), got) || got.size() != 2 ||
			got[0].address != "/bundle/a" || got[1].address != "/bundle/b") {
			return failCodec("smokeLoopback: parse bundle failed");
		}
	}

	hits = 0;
	const int lid = addListener("/rigosc/*", [&](const osc::OscRx& rx) {
		if (rx.msg.address == "/rigosc/ping") {
			++hits;
		}
	});
	if (lid <= 0 || !sendTo("127.0.0.1", port, "/rigosc/ping", {true})) {
		removeListener(lid);
		return failCodec("smokeLoopback: listener send failed");
	}
	bool heard = false;
	for (int i = 0; i < 20; ++i) {
		update(0.f);
		if (hits > 0) {
			heard = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	removeListener(lid);
	if (!heard) {
		return failCodec("smokeLoopback: listener did not fire");
	}

	const int ignoredBefore = m_ignoredPackets;
	const uint8_t garbage[] = {0x01, 0x02, 0x03, 0x04, 0xff, 0x00, 0x11, 0x22};
	if (!m_sock->sendTo("127.0.0.1", port, garbage, sizeof(garbage))) {
		return failCodec("smokeLoopback: garbage send failed");
	}
	bool ignored = false;
	for (int i = 0; i < 20; ++i) {
		update(0.f);
		if (m_ignoredPackets > ignoredBefore) {
			ignored = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	if (!ignored) {
		return failCodec("smokeLoopback: malformed packet not ignored");
	}

	spdlog::info("[rigOsc] smokeLoopback OK (directed+broadcast+bundle+listen port={})", port);
	restore();
	return true;
}

} // namespace rigkit

namespace {
struct rigOscRegistrar {
	rigOscRegistrar() {
		rigkit::PackRegistry::instance().addFactory("rigOsc", []() {
			return std::shared_ptr<rigkit::IPack>(std::make_shared<rigkit::rigOsc>());
		});
	}
};
static rigOscRegistrar rigOsc_auto_reg;
} // namespace
