#include "rigOsc.h"

#include "CNetworkIdentity.h"
#include "COscEndpoint.h"
#include "TcpSocket.h"
#include "UdpSocket.h"
#include "core/RigKitEngine.h"
#include "core/pack/PackRegistry.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <utility>

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

struct rigOsc::Net {
	struct Cmd {
		enum Kind { Apply, Connect, Send } kind = Send;
		NetConfig cfg;
		bool force = false;
		uint64_t token = 0;
		std::string host;
		int port = 0;
		std::vector<uint8_t> bytes;
		bool udpOnly = false;
	};

	std::mutex mu;
	std::condition_variable cv;
	std::thread thread;
	std::atomic<bool> stop{false};
	std::vector<Cmd> cmds;
	std::vector<osc::OscRx> inbound;
	osc::UdpSocket wake;
	osc::UdpSocket knock;
	int wakePort = 0;
	uint64_t nextToken = 1;
	uint64_t doneToken = 0;
	bool doneOk = false;
	std::string doneError;
	int doneListenPort = 0;
	bool doneRewriteListen = false;
	std::string lastError;
	NetConfig netCfg;
};

rigOsc::rigOsc() : IPack("rigOsc") {
	m_sock = std::make_unique<osc::UdpSocket>();
	m_tcp = std::make_unique<osc::TcpPort>();
	m_io = std::make_unique<Net>();
	startNet();
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
	stopNet();
	m_liveValid = false;
	m_udpOpen = false;
	m_tcpOpen = false;
	if (m_sock) {
		m_sock->close();
	}
	if (m_tcp) {
		m_tcp->close();
	}
	m_boundPort = -1;
	m_boundAddress.clear();
	m_boundGroup.clear();
	m_tcpPort = -1;
	m_tcpAddress.clear();
	m_due.clear();
	m_bindFailed = false;
	m_bindFailPort = -1;
	m_bindFailAddress.clear();
	m_bindFailGroup.clear();
	m_tcpBindFailed = false;
	m_tcpFailPort = -1;
	m_tcpFailAddress.clear();
	m_dialFailed = false;
	m_dialHost.clear();
	m_dialPort = -1;
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
	return m_udpOpen.load();
}

bool rigOsc::tcpListening() const {
	return m_tcpOpen.load();
}

std::string rigOsc::lastError() const {
	if (!m_io) {
		return m_lastError;
	}
	std::lock_guard lock(m_io->mu);
	if (!m_io->lastError.empty()) {
		return m_io->lastError;
	}
	return m_lastError;
}

rigOsc::NetConfig rigOsc::currentConfig() {
	ensureEntity();
	auto& ep = endpoint();
	auto& id = identity();
	NetConfig cfg;
	cfg.listenEnabled = ep.listenEnabled;
	cfg.listenPort = ep.listenPort;
	cfg.bindAddress = id.bindAddress;
	cfg.multicastGroup = ep.multicastGroup;
	cfg.multicastTtl = ep.multicastTtl;
	cfg.tcpListen = ep.tcpListen;
	cfg.tcpPort = ep.tcpPort;
	cfg.tcpConnect = ep.tcpConnect;
	cfg.sendEnabled = ep.sendEnabled;
	cfg.sendHost = ep.sendHost;
	cfg.sendPort = ep.sendPort;
	cfg.streamFraming = ep.streamFraming;
	return cfg;
}

bool rigOsc::applyEndpoint(bool force) {
	ensureEntity();
	auto& ep = endpoint();
	if (ep.multicastTtl < 0) {
		ep.multicastTtl = 0;
	}
	if (ep.multicastTtl > 255) {
		ep.multicastTtl = 255;
	}
	if (ep.streamFraming != osc::TcpPort::kFramingSlip) {
		ep.streamFraming = osc::TcpPort::kFramingSize;
	}
	const NetConfig cfg = currentConfig();
	if (!force && m_liveValid && cfg == m_liveCfg) {
		return m_liveOk;
	}
	std::string err;
	int listenPort = cfg.listenPort;
	bool rewrite = false;
	const bool ok = submitApply(cfg, force, err, listenPort, rewrite);
	NetConfig live = cfg;
	if (rewrite) {
		ep.listenPort = listenPort;
		live.listenPort = listenPort;
	}
	if (err == "osc thread did not answer") {
		m_liveValid = false;
		m_lastError = err;
		return false;
	}
	m_liveCfg = live;
	m_liveValid = true;
	m_liveOk = ok;
	m_lastError = err;
	if (m_io) {
		std::lock_guard lock(m_io->mu);
		m_io->lastError.clear();
	}
	return ok;
}

bool rigOsc::applyOnNet(const NetConfig& cfg, bool force, std::string& err, int& listenPort,
						bool& rewriteListen) {
	err.clear();
	rewriteListen = false;
	listenPort = cfg.listenPort;
	if (m_io) {
		m_io->netCfg = cfg;
	}
	if (!cfg.listenEnabled) {
		m_bindFailed = false;
		// Drop a listen bind; keep an ephemeral send socket if present.
		if (m_sock->isOpen() && m_boundPort.load() == cfg.listenPort) {
			m_sock->close();
			m_boundPort = -1;
			m_boundAddress.clear();
			m_boundGroup.clear();
		}
		m_udpOpen = false;
		if (m_tcp) {
			m_tcp->closeListen();
		}
		m_tcpPort = -1;
		m_tcpOpen = false;
		m_tcpAddress.clear();
		m_tcpBindFailed = false;
		if (m_io) {
			m_io->netCfg = cfg;
		}
		maintainTcpConnect(cfg);
		return true;
	}

	const bool udpSame = m_sock->isOpen() && m_boundPort.load() == cfg.listenPort &&
						 m_boundAddress == cfg.bindAddress && m_boundGroup == cfg.multicastGroup &&
						 m_boundTtl == cfg.multicastTtl;
	if (!udpSame) {
		if (!force && m_bindFailed && m_bindFailPort == cfg.listenPort &&
			m_bindFailAddress == cfg.bindAddress && m_bindFailGroup == cfg.multicastGroup) {
			err = m_sock->lastError().empty() ? "bind failed" : m_sock->lastError();
			m_udpOpen = false;
			return false;
		}
		if (!m_sock->bind(cfg.bindAddress, cfg.listenPort, cfg.multicastGroup, cfg.multicastTtl)) {
			err = m_sock->lastError();
			m_bindFailed = true;
			m_bindFailPort = cfg.listenPort;
			m_bindFailAddress = cfg.bindAddress;
			m_bindFailGroup = cfg.multicastGroup;
			m_udpOpen = false;
			spdlog::error("[rigOsc] bind {}:{} failed - {}", cfg.bindAddress, cfg.listenPort, err);
			return false;
		}
		m_bindFailed = false;
		m_bindFailPort = -1;
		m_bindFailAddress.clear();
		m_bindFailGroup.clear();
		// Port 0 to kernel picks ephemeral; record the real bound port.
		m_boundPort = m_sock->port();
		if (cfg.listenPort == 0) {
			rewriteListen = true;
			listenPort = m_boundPort.load();
		}
		m_boundAddress = cfg.bindAddress;
		m_boundGroup = cfg.multicastGroup;
		m_boundTtl = cfg.multicastTtl;
		spdlog::info("[rigOsc] listening on {}:{}", cfg.bindAddress, m_boundPort.load());
		if (!cfg.multicastGroup.empty()) {
			spdlog::info("[rigOsc] multicast {} ttl {}", cfg.multicastGroup, cfg.multicastTtl);
		}
	}
	m_udpOpen = true;

	const int udpPort = m_boundPort.load();
	const int tcpWant = cfg.tcpListen ? (cfg.tcpPort > 0 ? cfg.tcpPort : udpPort) : -1;
	const int framing = cfg.streamFraming == osc::TcpPort::kFramingSlip
							? osc::TcpPort::kFramingSlip
							: osc::TcpPort::kFramingSize;
	const bool tcpSame = tcpWant < 0 ? (m_tcpPort.load() < 0)
									 : (m_tcp->isListening() && m_tcpPort.load() == tcpWant &&
										m_tcpAddress == cfg.bindAddress && m_tcpFraming == framing);
	if (!tcpSame) {
		if (tcpWant < 0) {
			m_tcp->closeListen();
			m_tcpPort = -1;
			m_tcpOpen = false;
			m_tcpAddress.clear();
			m_tcpBindFailed = false;
		} else if (!force && m_tcpBindFailed && m_tcpFailPort == tcpWant &&
				   m_tcpFailAddress == cfg.bindAddress) {
			err = m_tcp->lastError().empty() ? "tcp bind failed" : m_tcp->lastError();
			return false;
		} else if (!m_tcp->listen(cfg.bindAddress, tcpWant)) {
			err = m_tcp->lastError();
			m_tcpBindFailed = true;
			m_tcpFailPort = tcpWant;
			m_tcpFailAddress = cfg.bindAddress;
			m_tcpOpen = false;
			spdlog::error("[rigOsc] tcp {}:{} failed - {}", cfg.bindAddress, tcpWant, err);
			return false;
		} else {
			m_tcpBindFailed = false;
			m_tcpFailPort = -1;
			m_tcpFailAddress.clear();
			m_tcpPort = m_tcp->port();
			m_tcpOpen = true;
			m_tcpAddress = cfg.bindAddress;
			m_tcpFraming = framing;
			spdlog::info("[rigOsc] tcp {}:{} framing {}", cfg.bindAddress, m_tcpPort.load(),
						 m_tcpFraming == osc::TcpPort::kFramingSlip ? "slip" : "size");
		}
	} else {
		m_tcpOpen = tcpWant >= 0;
	}
	if (m_io) {
		m_io->netCfg = cfg;
		if (rewriteListen) {
			m_io->netCfg.listenPort = listenPort;
		}
	}
	maintainTcpConnect(cfg);
	return true;
}

const char* rigOsc::commandLineHelp() {
	return "  --network-id=ID / --id=ID   OSC network id (default: install)\n"
		   "  --listen-port=N             UDP listen port (default: 8000)\n"
		   "  --send-host=HOST            Send host (default: 127.0.0.1)\n"
		   "  --send-port=N               Send port (default: 8001)\n"
		   "  --bind-address=ADDR         Listen bind (default: 0.0.0.0)\n"
		   "  --tcp                       TCP listen (port = listen port)\n"
		   "  --tcp-port=N                TCP listen port (0 = listen port)\n"
		   "  --slip                      TCP framing SLIP (default: size prefix)\n"
		   "  --multicast=GROUP           IPv4 multicast group to join\n"
		   "  --multicast-ttl=N           Multicast TTL (default: 1)";
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
	if (args.hasFlag("tcp")) {
		ep.tcpListen = true;
	}
	if (auto v = args.getValue("tcp-port")) {
		ep.tcpListen = true;
		ep.tcpPort = std::stoi(*v);
	}
	if (args.hasFlag("slip")) {
		ep.streamFraming = osc::TcpPort::kFramingSlip;
	}
	if (auto v = args.getValue("multicast")) {
		ep.multicastGroup = *v;
	}
	if (auto v = args.getValue("multicast-ttl")) {
		ep.multicastTtl = std::stoi(*v);
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

void rigOsc::deliver(osc::OscRx rx) {
	m_lastRx = rx;
	m_haveRx = true;
	dispatch(rx);
}

void rigOsc::offer(osc::OscRx rx) {
	const osc::OscTimetag now = osc::timetagNow();
	if (rx.msg.time.isImmediate() || osc::timetagCompare(rx.msg.time, now) <= 0) {
		deliver(std::move(rx));
		return;
	}
	m_due.push_back(std::move(rx));
}

void rigOsc::flushDue() {
	if (m_due.empty()) {
		return;
	}
	const osc::OscTimetag now = osc::timetagNow();
	size_t keep = 0;
	for (size_t i = 0; i < m_due.size(); ++i) {
		if (osc::timetagCompare(m_due[i].msg.time, now) <= 0) {
			deliver(std::move(m_due[i]));
		} else {
			if (keep != i) {
				m_due[keep] = std::move(m_due[i]);
			}
			++keep;
		}
	}
	m_due.resize(keep);
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
	std::vector<uint8_t> bytes;
	if (!osc::encodeMessage(address, args, bytes)) {
		m_lastError = "encode failed";
		return false;
	}
	enqueueSend(host, port, std::move(bytes), false);
	return true;
}

bool rigOsc::connectTcp(const std::string& host, int port) {
	if (host.empty() || port <= 0 || port > 65535) {
		m_lastError = "tcp connect failed";
		return false;
	}
	enqueueConnect(host, port);
	return true;
}

void rigOsc::maintainTcpConnect(const NetConfig& cfg) {
	if (!cfg.tcpConnect || !cfg.sendEnabled || cfg.sendHost.empty() || cfg.sendPort <= 0) {
		return;
	}
	if (m_tcp->hasPeer(cfg.sendHost, cfg.sendPort)) {
		return;
	}
	if (m_dialFailed && m_dialHost == cfg.sendHost && m_dialPort == cfg.sendPort) {
		return;
	}
	if (!m_tcp->connect(cfg.sendHost, cfg.sendPort)) {
		m_dialFailed = true;
		m_dialHost = cfg.sendHost;
		m_dialPort = cfg.sendPort;
		spdlog::error("[rigOsc] tcp dial {}:{} failed - {}", cfg.sendHost, cfg.sendPort,
					  m_tcp->lastError());
	}
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
	flushDue();
	if (!m_io) {
		return;
	}
	std::vector<osc::OscRx> inbound;
	{
		std::lock_guard lock(m_io->mu);
		inbound.swap(m_io->inbound);
	}
	for (osc::OscRx& rx : inbound) {
		offer(std::move(rx));
	}
}

void rigOsc::startNet() {
	if (!m_io || m_io->thread.joinable()) {
		return;
	}
	m_io->stop = false;
	if (m_io->wake.bind("127.0.0.1", 0)) {
		m_io->wakePort = m_io->wake.port();
		m_io->knock.bind("127.0.0.1", 0);
	}
	m_io->thread = std::thread([this] { netLoop(); });
}

void rigOsc::stopNet() {
	if (!m_io || !m_io->thread.joinable()) {
		return;
	}
	m_io->stop = true;
	wakeNet();
	m_io->cv.notify_all();
	m_io->thread.join();
}

void rigOsc::wakeNet() {
	if (!m_io || !m_io->knock.isOpen() || m_io->wakePort <= 0) {
		return;
	}
	const uint8_t byte = 1;
	m_io->knock.sendTo("127.0.0.1", m_io->wakePort, &byte, 1);
}

bool rigOsc::submitApply(const NetConfig& cfg, bool force, std::string& err, int& listenPort,
						 bool& rewriteListen) {
	if (!m_io || !m_io->thread.joinable()) {
		err = "osc thread is not running";
		return false;
	}
	uint64_t token = 0;
	{
		std::lock_guard lock(m_io->mu);
		token = m_io->nextToken++;
		Net::Cmd cmd;
		cmd.kind = Net::Cmd::Apply;
		cmd.cfg = cfg;
		cmd.force = force;
		cmd.token = token;
		m_io->cmds.push_back(std::move(cmd));
	}
	wakeNet();
	std::unique_lock lock(m_io->mu);
	const bool answered = m_io->cv.wait_for(lock, std::chrono::seconds(2), [&] {
		return m_io->doneToken == token || m_io->stop.load();
	});
	if (!answered || m_io->doneToken != token) {
		err = "osc thread did not answer";
		return false;
	}
	err = m_io->doneError;
	listenPort = m_io->doneListenPort;
	rewriteListen = m_io->doneRewriteListen;
	return m_io->doneOk;
}

void rigOsc::enqueueSend(std::string host, int port, std::vector<uint8_t> bytes, bool udpOnly) {
	if (!m_io) {
		return;
	}
	Net::Cmd cmd;
	cmd.kind = Net::Cmd::Send;
	cmd.host = std::move(host);
	cmd.port = port;
	cmd.bytes = std::move(bytes);
	cmd.udpOnly = udpOnly;
	{
		std::lock_guard lock(m_io->mu);
		m_io->cmds.push_back(std::move(cmd));
	}
	wakeNet();
}

void rigOsc::enqueueConnect(std::string host, int port) {
	if (!m_io) {
		return;
	}
	Net::Cmd cmd;
	cmd.kind = Net::Cmd::Connect;
	cmd.host = std::move(host);
	cmd.port = port;
	{
		std::lock_guard lock(m_io->mu);
		m_io->cmds.push_back(std::move(cmd));
	}
	wakeNet();
}

bool rigOsc::sendDatagram(const std::string& host, int port, const uint8_t* data, size_t len) {
	if (!data || len == 0 || port <= 0) {
		return false;
	}
	enqueueSend(host, port, std::vector<uint8_t>(data, data + len), true);
	return true;
}

void rigOsc::drainSockets() {
	std::vector<osc::OscRx> got;
	auto take = [&](std::vector<osc::OscMessage>& msgs, const std::string& host, int port) {
		if (msgs.empty()) {
			m_ignoredPackets.fetch_add(1);
			return;
		}
		for (osc::OscMessage& msg : msgs) {
			osc::OscRx rx;
			rx.msg = std::move(msg);
			rx.fromHost = host;
			rx.fromPort = port;
			got.push_back(std::move(rx));
		}
	};
	if (m_sock && m_sock->isOpen()) {
		while (true) {
			std::string fromHost;
			int fromPort = 0;
			m_rxBuf.clear();
			if (m_sock->recvFrom(m_rxBuf, &fromHost, &fromPort) == 0) {
				break;
			}
			std::vector<osc::OscMessage> msgs;
			if (!osc::parsePacket(m_rxBuf.data(), m_rxBuf.size(), msgs)) {
				m_ignoredPackets.fetch_add(1);
				continue;
			}
			take(msgs, fromHost, fromPort);
		}
	}
	if (m_tcp) {
		const int before = m_tcp->discarded();
		const int framing = m_io && m_io->netCfg.streamFraming == osc::TcpPort::kFramingSlip
								? osc::TcpPort::kFramingSlip
								: osc::TcpPort::kFramingSize;
		std::vector<osc::TcpPacket> pkts;
		m_tcp->poll(framing, pkts);
		m_ignoredPackets.fetch_add(m_tcp->discarded() - before);
		for (osc::TcpPacket& pkt : pkts) {
			std::vector<osc::OscMessage> msgs;
			if (!osc::parsePacket(pkt.bytes.data(), pkt.bytes.size(), msgs)) {
				m_ignoredPackets.fetch_add(1);
				continue;
			}
			take(msgs, pkt.fromHost, pkt.fromPort);
		}
	}
	if (!got.empty() && m_io) {
		std::lock_guard lock(m_io->mu);
		m_io->inbound.insert(m_io->inbound.end(), std::make_move_iterator(got.begin()),
							 std::make_move_iterator(got.end()));
	}
}

void rigOsc::netLoop() {
	while (m_io && !m_io->stop.load()) {
		std::vector<Net::Cmd> cmds;
		{
			std::lock_guard lock(m_io->mu);
			cmds.swap(m_io->cmds);
		}
		for (Net::Cmd& cmd : cmds) {
			if (cmd.kind == Net::Cmd::Apply) {
				std::string err;
				int port = 0;
				bool rewrite = false;
				const bool ok = applyOnNet(cmd.cfg, cmd.force, err, port, rewrite);
				{
					std::lock_guard lock(m_io->mu);
					m_io->doneToken = cmd.token;
					m_io->doneOk = ok;
					m_io->doneError = std::move(err);
					m_io->doneListenPort = port;
					m_io->doneRewriteListen = rewrite;
				}
				m_io->cv.notify_all();
			} else if (cmd.kind == Net::Cmd::Connect) {
				m_dialFailed = false;
				if (!m_tcp->connect(cmd.host, cmd.port)) {
					spdlog::error("[rigOsc] tcp connect {}:{} failed - {}", cmd.host, cmd.port,
								  m_tcp->lastError());
				}
			} else if (!cmd.bytes.empty()) {
				if (!m_sock->isOpen()) {
					const std::string bindOn = m_io->netCfg.bindAddress.empty()
												   ? std::string("0.0.0.0")
												   : m_io->netCfg.bindAddress;
					if (!m_sock->bind(bindOn, 0)) {
						spdlog::error("[rigOsc] send bind failed - {}", m_sock->lastError());
						continue;
					}
					m_boundPort = m_sock->port();
					m_boundAddress = bindOn;
				}
				const int framing = m_io->netCfg.streamFraming == osc::TcpPort::kFramingSlip
										? osc::TcpPort::kFramingSlip
										: osc::TcpPort::kFramingSize;
				if (!cmd.udpOnly && m_tcp->hasPeer(cmd.host, cmd.port)) {
					if (!m_tcp->sendTo(cmd.host, cmd.port, cmd.bytes.data(), cmd.bytes.size(),
									   framing)) {
						spdlog::error("[rigOsc] tcp send failed - {}", m_tcp->lastError());
					}
				} else if (!m_sock->sendTo(cmd.host, cmd.port, cmd.bytes.data(),
										   cmd.bytes.size())) {
					spdlog::error("[rigOsc] udp send failed - {}", m_sock->lastError());
				}
			}
		}
		if (m_io->netCfg.tcpConnect) {
			maintainTcpConnect(m_io->netCfg);
		}
		drainSockets();
		if (m_io->stop.load()) {
			break;
		}
		bool more = false;
		{
			std::lock_guard lock(m_io->mu);
			more = !m_io->cmds.empty();
		}
		if (more) {
			continue;
		}
		std::vector<osc::SockInterest> wait;
		if (m_io->wake.isOpen()) {
			wait.push_back(osc::SockInterest{m_io->wake.handle(), true, false});
		}
		if (m_sock->isOpen()) {
			wait.push_back(osc::SockInterest{m_sock->handle(), true, false});
		}
		if (m_tcp) {
			m_tcp->interests(wait);
		}
		osc::waitSockets(wait, 200);
		if (m_io->wake.isOpen()) {
			std::vector<uint8_t> dump;
			while (m_io->wake.recvFrom(dump) > 0) {
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
	const bool savedTcp = ep.tcpListen;
	const bool savedTcpConnect = ep.tcpConnect;
	const int savedTcpPort = ep.tcpPort;
	const int savedFraming = ep.streamFraming;
	const std::string savedGroup = ep.multicastGroup;
	const int savedTtl = ep.multicastTtl;

	auto restore = [&]() {
		ep.listenEnabled = savedListenEn;
		ep.sendEnabled = savedSendEn;
		ep.listenPort = savedListenPort;
		ep.sendPort = savedSendPort;
		ep.sendHost = savedSendHost;
		id.bindAddress = savedBind;
		ep.tcpListen = savedTcp;
		ep.tcpConnect = savedTcpConnect;
		ep.tcpPort = savedTcpPort;
		ep.streamFraming = savedFraming;
		ep.multicastGroup = savedGroup;
		ep.multicastTtl = savedTtl;
		applyEndpoint(true);
	};

	ep.listenEnabled = true;
	ep.sendEnabled = true;
	ep.sendHost = "127.0.0.1";
	id.bindAddress = "127.0.0.1";
	ep.tcpListen = false;
	ep.tcpConnect = false;
	ep.tcpPort = 0;
	ep.streamFraming = osc::TcpPort::kFramingSize;
	ep.multicastGroup.clear();
	ep.multicastTtl = 1;
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
	if (!sendDatagram("127.0.0.1", port, pkt.data(), pkt.size())) {
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
	if (!sendDatagram("127.0.0.1", port, pkt.data(), pkt.size())) {
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
		!sendDatagram("127.0.0.1", port, pkt.data(), pkt.size()) || !waitGot(0.77f, "other")) {
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
			got[0].address != "/bundle/a" || got[1].address != "/bundle/b" ||
			!got[0].time.isImmediate()) {
			return failCodec("smokeLoopback: parse bundle failed");
		}
	}

	{
		const int64_t wide = int64_t(1) << 40;
		const double precise = 1.5;
		osc::OscTimetag tag = osc::OscTimetag::fromBits(0x0000000100000002ull);
		pkt.clear();
		if (!osc::encodeMessage("/wide", {wide, precise, osc::OscNil{}, osc::OscSymbol{"sym"}, tag},
								pkt)) {
			return failCodec("smokeLoopback: encode wide failed");
		}
		osc::OscMessage wideGot;
		if (!osc::parsePacket(pkt.data(), pkt.size(), wideGot) || wideGot.args.size() != 5) {
			return failCodec("smokeLoopback: parse wide failed");
		}
		const auto* hi = std::get_if<int64_t>(&wideGot.args[0]);
		const auto* dd = std::get_if<double>(&wideGot.args[1]);
		const auto* sym = std::get_if<osc::OscSymbol>(&wideGot.args[3]);
		const auto* tt = std::get_if<osc::OscTimetag>(&wideGot.args[4]);
		if (!hi || *hi != wide || !dd || *dd != precise ||
			!std::holds_alternative<osc::OscNil>(wideGot.args[2]) || !sym || sym->value != "sym" ||
			!tt || tt->bits() != tag.bits()) {
			return failCodec("smokeLoopback: wide roundtrip failed");
		}
	}

	{
		osc::OscArray inner;
		osc::items(inner).push_back(int32_t{7});
		osc::items(inner).push_back(8.f);
		osc::OscArray outer;
		osc::items(outer).push_back(std::move(inner));
		osc::items(outer).push_back(int32_t{9});
		pkt.clear();
		if (!osc::encodeMessage("/arr", {outer}, pkt)) {
			return failCodec("smokeLoopback: encode array failed");
		}
		osc::OscMessage arrGot;
		if (!osc::parsePacket(pkt.data(), pkt.size(), arrGot) || arrGot.args.size() != 1) {
			return failCodec("smokeLoopback: parse array failed");
		}
		const auto* gotOuter = std::get_if<osc::OscArray>(&arrGot.args[0]);
		if (!gotOuter || osc::items(*gotOuter).size() != 2) {
			return failCodec("smokeLoopback: array shape failed");
		}
		const auto* gotInner = std::get_if<osc::OscArray>(&osc::items(*gotOuter)[0]);
		const auto* gotNine = std::get_if<int32_t>(&osc::items(*gotOuter)[1]);
		const auto* gotSeven = gotInner ? std::get_if<int32_t>(&osc::items(*gotInner)[0]) : nullptr;
		const auto* gotEight = gotInner ? std::get_if<float>(&osc::items(*gotInner)[1]) : nullptr;
		if (!gotInner || osc::items(*gotInner).size() != 2 || !gotSeven || *gotSeven != 7 ||
			!gotEight || *gotEight != 8.f || !gotNine || *gotNine != 9) {
			return failCodec("smokeLoopback: array roundtrip failed");
		}
	}

	auto tcpRoundtrip = [&](int framing, const char* label) -> bool {
		ep.tcpListen = true;
		ep.tcpPort = 0;
		ep.tcpConnect = false;
		ep.streamFraming = framing;
		if (!applyEndpoint(true) || tcpBoundPort() <= 0) {
			m_lastError = std::string("smokeLoopback: tcp bind failed (") + label + ")";
			spdlog::error("[rigOsc] {}", m_lastError);
			restore();
			return false;
		}
		const int tcpPort = tcpBoundPort();
		if (!connectTcp("127.0.0.1", tcpPort)) {
			m_lastError = std::string("smokeLoopback: tcp connect failed (") + label + ")";
			spdlog::error("[rigOsc] {}", m_lastError);
			restore();
			return false;
		}
		int tcpHits = 0;
		const int tcpId = addListener("/rigosc/tcp", [&](const osc::OscRx& rx) {
			if (rx.msg.args.size() == 1 && rx.fromPort > 0) {
				++tcpHits;
			}
		});
		bool heardTcp = false;
		for (int i = 0; i < 40; ++i) {
			sendTo("127.0.0.1", tcpPort, "/rigosc/tcp", {int32_t{1}});
			update(0.f);
			if (tcpHits > 0) {
				heardTcp = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		removeListener(tcpId);
		ep.tcpListen = false;
		applyEndpoint(true);
		if (!heardTcp) {
			m_lastError = std::string("smokeLoopback: tcp loopback failed (") + label + ")";
			spdlog::error("[rigOsc] {}", m_lastError);
			restore();
			return false;
		}
		return true;
	};
	if (!tcpRoundtrip(osc::TcpPort::kFramingSize, "size") ||
		!tcpRoundtrip(osc::TcpPort::kFramingSlip, "slip")) {
		return false;
	}

	{
		osc::OscMessage later;
		later.address = "/rigosc/later";
		later.args.push_back(1);
		const uint64_t dueBits =
			osc::timetagNow().bits() + static_cast<uint64_t>(0.12 * 4294967296.0);
		pkt.clear();
		if (!osc::encodeBundle({later}, osc::OscTimetag::fromBits(dueBits), pkt)) {
			return failCodec("smokeLoopback: encode timed bundle failed");
		}
		int late = 0;
		const int lateId = addListener("/rigosc/later", [&](const osc::OscRx&) { ++late; });
		if (lateId <= 0 || !sendDatagram("127.0.0.1", port, pkt.data(), pkt.size())) {
			removeListener(lateId);
			return failCodec("smokeLoopback: timed bundle send failed");
		}
		for (int i = 0; i < 4; ++i) {
			update(0.f);
		}
		if (late != 0) {
			removeListener(lateId);
			return failCodec("smokeLoopback: future bundle fired early");
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		update(0.f);
		removeListener(lateId);
		if (late != 1) {
			return failCodec("smokeLoopback: future bundle did not fire");
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
	if (!sendDatagram("127.0.0.1", port, garbage, sizeof(garbage))) {
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

	ep.multicastGroup = "239.255.42.99";
	ep.multicastTtl = 1;
	ep.tcpListen = false;
	ep.tcpConnect = false;
	id.bindAddress = "0.0.0.0";
	ep.listenPort = 0;
	if (!applyEndpoint(true)) {
		return failCodec("smokeLoopback: multicast bind failed");
	}
	const int mport = m_boundPort;
	int mhits = 0;
	const int mid = addListener("/rigosc/mcast", [&](const osc::OscRx&) { ++mhits; });
	bool heardM = false;
	for (int i = 0; i < 20; ++i) {
		if (!sendTo("239.255.42.99", mport, "/rigosc/mcast", {int32_t{1}})) {
			break;
		}
		update(0.f);
		if (mhits > 0) {
			heardM = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	removeListener(mid);
	if (!heardM) {
		return failCodec("smokeLoopback: multicast loopback failed");
	}

	spdlog::info("[rigOsc] smokeLoopback OK (udp+tcp+multicast port={})", port);
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
