#include "rigOsc.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <spdlog/spdlog.h>
#include "CNetworkIdentity.h"
#include "COscEndpoint.h"
#include "COscShowBus.h"
#include "UdpSocket.h"
#include "core/RigKitEngine.h"
#include "core/pack/PackRegistry.h"

namespace rigkit {

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
	if (!ecs->hasComponent<ecs::COscShowBus>(m_entity)) {
		ecs->addComponent<ecs::COscShowBus>(m_entity, {});
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
	ecs->registerComponent<ecs::COscShowBus>("OscShowBus", true);
	ensureEntity();
	applyEndpoint();
	spdlog::info("[rigOsc] setup — id='{}' listen {}:{}", identity().networkId,
				 identity().bindAddress, endpoint().listenPort);
}

void rigOsc::cleanup() {
	if (m_sock) {
		m_sock->close();
	}
	m_boundPort = -1;
	m_boundAddress.clear();
}

ecs::CNetworkIdentity& rigOsc::identity() {
	ensureEntity();
	return getEngine()->getECSManager()->getComponent<ecs::CNetworkIdentity>(m_entity);
}

ecs::COscEndpoint& rigOsc::endpoint() {
	ensureEntity();
	return getEngine()->getECSManager()->getComponent<ecs::COscEndpoint>(m_entity);
}

ecs::COscShowBus& rigOsc::showBus() {
	ensureEntity();
	return getEngine()->getECSManager()->getComponent<ecs::COscShowBus>(m_entity);
}

bool rigOsc::listening() const {
	return m_sock && m_sock->isOpen();
}

bool rigOsc::applyEndpoint() {
	ensureEntity();
	m_lastError.clear();
	auto& ep = endpoint();
	auto& id = identity();
	if (!ep.listenEnabled) {
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
	if (!m_sock->bind(id.bindAddress, ep.listenPort)) {
		m_lastError = m_sock->lastError();
		spdlog::error("[rigOsc] bind {}:{} failed — {}", id.bindAddress, ep.listenPort, m_lastError);
		return false;
	}
	// Port 0 → kernel picks ephemeral; record the real bound port.
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
				 id.bindAddress, ep.listenPort, ep.sendHost, ep.sendPort, listening() ? "yes" : "no");
	return ok;
}

std::string rigOsc::makeAddress(const std::string& leaf) const {
	auto* self = const_cast<rigOsc*>(this);
	const auto& ep = self->endpoint();
	const auto& id = self->identity();
	std::string prefix = ep.addressPrefix.empty() ? "/rigkit" : ep.addressPrefix;
	if (prefix.back() == '/') {
		prefix.pop_back();
	}
	return prefix + "/" + id.networkId + "/" + leaf;
}

std::string rigOsc::makeBroadcastAddress(const std::string& leaf) const {
	auto* self = const_cast<rigOsc*>(this);
	const auto& ep = self->endpoint();
	std::string prefix = ep.addressPrefix.empty() ? "/rigkit" : ep.addressPrefix;
	if (prefix.back() == '/') {
		prefix.pop_back();
	}
	return prefix + "/" + leaf;
}

std::string rigOsc::leafAddress(const std::string& full) const {
	auto* self = const_cast<rigOsc*>(this);
	const auto& ep = self->endpoint();
	const auto& id = self->identity();
	std::string prefix = ep.addressPrefix.empty() ? "/rigkit" : ep.addressPrefix;
	if (prefix.back() == '/') {
		prefix.pop_back();
	}
	const std::string withId = prefix + "/" + id.networkId + "/";
	const std::string bare = prefix + "/";
	if (full.rfind(withId, 0) == 0) {
		return full.substr(withId.size());
	}
	// Broadcast: /prefix/<leaf> with no network id segment (leaf must not contain '/').
	if (full.rfind(bare, 0) == 0) {
		const std::string rest = full.substr(bare.size());
		if (rest.find('/') == std::string::npos) {
			return rest;
		}
	}
	return {};
}

void rigOsc::noteSender(const osc::OscMessage& msg) {
	// Trailing string arg is the sender network id (apps append it on bus sends).
	if (msg.args.size() < 2) {
		return;
	}
	if (const auto* s = std::get_if<std::string>(&msg.args.back())) {
		if (!s->empty()) {
			showBus().lastFrom = *s;
		}
	}
}

void rigOsc::handleMessage(const osc::OscMessage& msg) {
	const std::string leaf = leafAddress(msg.address);
	if (leaf.empty()) {
		return;
	}
	auto& bus = showBus();
	auto asFloat = [&](float& dst) -> bool {
		if (msg.args.empty()) {
			return false;
		}
		if (const auto* f = std::get_if<float>(&msg.args[0])) {
			dst = *f;
			return true;
		}
		if (const auto* i = std::get_if<int32_t>(&msg.args[0])) {
			dst = static_cast<float>(*i);
			return true;
		}
		return false;
	};
	auto asBool = [&](bool& dst) -> bool {
		if (msg.args.empty()) {
			return false;
		}
		if (const auto* b = std::get_if<bool>(&msg.args[0])) {
			dst = *b;
			return true;
		}
		if (const auto* i = std::get_if<int32_t>(&msg.args[0])) {
			dst = (*i) != 0;
			return true;
		}
		if (const auto* f = std::get_if<float>(&msg.args[0])) {
			dst = (*f) >= 0.5f;
			return true;
		}
		return false;
	};

	if (leaf == "master") {
		if (asFloat(bus.master)) {
			bus.master = std::clamp(bus.master, 0.f, 1.f);
			bus.masterFromNet = true;
			noteSender(msg);
		}
	} else if (leaf == "blackout") {
		if (asBool(bus.blackout)) {
			bus.blackoutFromNet = true;
			noteSender(msg);
		}
	} else if (leaf == "color") {
		if (msg.args.size() >= 3) {
			auto take = [&](size_t i, float& dst) -> bool {
				if (const auto* f = std::get_if<float>(&msg.args[i])) {
					dst = *f;
					return true;
				}
				if (const auto* iv = std::get_if<int32_t>(&msg.args[i])) {
					dst = static_cast<float>(*iv);
					return true;
				}
				return false;
			};
			if (take(0, bus.colorR) && take(1, bus.colorG) && take(2, bus.colorB)) {
				bus.colorR = std::clamp(bus.colorR, 0.f, 1.f);
				bus.colorG = std::clamp(bus.colorG, 0.f, 1.f);
				bus.colorB = std::clamp(bus.colorB, 0.f, 1.f);
				bus.colorFromNet = true;
				noteSender(msg);
			}
		}
	} else if (leaf == "status") {
		if (!msg.args.empty()) {
			if (const auto* s = std::get_if<std::string>(&msg.args[0])) {
				bus.status = *s;
				bus.statusFromNet = true;
				noteSender(msg);
			}
		}
	} else if (leaf == "heartbeat") {
		if (!msg.args.empty()) {
			if (const auto* i = std::get_if<int32_t>(&msg.args[0])) {
				bus.heartbeat = *i;
				noteSender(msg);
			}
		}
	}
}

bool rigOsc::send(const std::string& address, const std::vector<osc::OscArg>& args) {
	ensureEntity();
	auto& ep = endpoint();
	if (!ep.sendEnabled) {
		return false;
	}
	if (!m_sock->isOpen()) {
		// Send-only: bind ephemeral UDP port.
		if (!m_sock->bind(identity().bindAddress, 0)) {
			m_lastError = m_sock->lastError();
			return false;
		}
		m_boundPort = m_sock->port();
		m_boundAddress = identity().bindAddress;
	}
	std::vector<uint8_t> pkt;
	if (!osc::encodeMessage(address, args, pkt)) {
		m_lastError = "encode failed";
		return false;
	}
	if (!m_sock->sendTo(ep.sendHost, ep.sendPort, pkt.data(), pkt.size())) {
		m_lastError = m_sock->lastError();
		return false;
	}
	return true;
}

void rigOsc::maybeSendHeartbeat() {
	auto& ep = endpoint();
	if (!ep.sendEnabled) {
		return;
	}
	auto& bus = showBus();
	if (bus.heartbeat == m_lastSentHeartbeat) {
		return;
	}
	m_lastSentHeartbeat = bus.heartbeat;
	const std::string& id = identity().networkId;
	send(makeAddress("heartbeat"), {static_cast<int32_t>(bus.heartbeat), id});
	send(makeAddress("status"), {bus.status, id});
	send(makeAddress("master"), {bus.master, id});
	send(makeAddress("blackout"), {bus.blackout, id});
	send(makeAddress("color"), {bus.colorR, bus.colorG, bus.colorB, id});
}

void rigOsc::update(float deltaTime) {
	ensureEntity();
	applyEndpoint();

	if (m_sock->isOpen()) {
		for (int i = 0; i < 32; ++i) {
			std::vector<uint8_t> pkt;
			if (m_sock->recvFrom(pkt) == 0) {
				break;
			}
			osc::OscMessage msg;
			if (osc::parsePacket(pkt.data(), pkt.size(), msg)) {
				handleMessage(msg);
			}
		}
	}

	m_sendAccum += deltaTime;
	if (m_sendAccum >= 1.f) {
		m_sendAccum = 0.f;
		maybeSendHeartbeat();
	}
}

bool rigOsc::smokeLoopback() {
	ensureEntity();
	auto& ep = endpoint();
	auto& id = identity();

	// Snapshot caller endpoint — smoke must not steal a peer's listen port.
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
	ep.listenPort = 0; // ephemeral — never collide with another instance on 8000
	if (!applyEndpoint()) {
		restore();
		return false;
	}
	const int port = m_boundPort;
	ep.sendPort = port;

	auto& bus = showBus();
	auto waitMaster = [&](float expect) -> bool {
		for (int i = 0; i < 20; ++i) {
			update(0.f);
			if (bus.masterFromNet && std::abs(bus.master - expect) < 0.001f) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return false;
	};

	bus.master = 0.f;
	bus.masterFromNet = false;
	bus.lastFrom.clear();
	std::vector<uint8_t> pkt;
	if (!osc::encodeMessage(makeAddress("master"), {0.42f, std::string(identity().networkId)},
							pkt)) {
		m_lastError = "encode smoke failed";
		restore();
		return false;
	}
	if (!m_sock->sendTo("127.0.0.1", port, pkt.data(), pkt.size())) {
		m_lastError = m_sock->lastError().empty() ? "sendTo failed" : m_sock->lastError();
		restore();
		return false;
	}
	if (!waitMaster(0.42f)) {
		m_lastError = "smokeLoopback: no packet received (directed)";
		spdlog::error("[rigOsc] {}", m_lastError);
		restore();
		return false;
	}

	bus.master = 0.f;
	bus.masterFromNet = false;
	bus.lastFrom.clear();
	pkt.clear();
	if (!osc::encodeMessage(makeBroadcastAddress("master"),
							{0.55f, std::string(identity().networkId)}, pkt)) {
		m_lastError = "encode broadcast smoke failed";
		restore();
		return false;
	}
	if (!m_sock->sendTo("127.0.0.1", port, pkt.data(), pkt.size())) {
		m_lastError = m_sock->lastError().empty() ? "sendTo failed" : m_sock->lastError();
		restore();
		return false;
	}
	if (!waitMaster(0.55f)) {
		m_lastError = "smokeLoopback: no packet received (broadcast)";
		spdlog::error("[rigOsc] {}", m_lastError);
		restore();
		return false;
	}
	if (bus.lastFrom != identity().networkId) {
		m_lastError = "smokeLoopback: sender id not recorded";
		spdlog::error("[rigOsc] {}", m_lastError);
		restore();
		return false;
	}

	spdlog::info("[rigOsc] smokeLoopback OK (directed+broadcast port={})", port);
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
