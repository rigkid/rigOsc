#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/pack/IPack.h"
#include "core/util/CommandLineArgs.h"
#include "ecs/MEcs.h"
#include "OscCodec.h"

namespace rigkit {

namespace ecs {
struct CNetworkIdentity;
struct COscEndpoint;
struct COscShowBus;
}

namespace osc {
class UdpSocket;
}

/**
 * @brief UDP OSC transport pack for installs.
 * @details Owns sockets; PODs hold identity / endpoints / show bus. Directed
 * addresses are `/rigkit/` then `networkId` then `master` / `blackout` /
 * `color` / `status` / `heartbeat`. Broadcast is `/rigkit/` then the leaf, no
 * id. Bus sends append sender network id as a trailing string arg.
 *
 * After pack setup, product apps call @ref applyCommandLine so network id / ports
 * come from the same CLI everywhere (not only examples).
 */
class rigOsc : public IPack {
  public:
	rigOsc();
	~rigOsc() override;

	bool init() override;
	void setup() override;
	void update(float deltaTime) override;
	void cleanup() override;

	entt::entity configEntity() const { return m_entity; }

	ecs::CNetworkIdentity& identity();
	ecs::COscEndpoint& endpoint();
	ecs::COscShowBus& showBus();

	/** @brief Rebind listen socket from current endpoint/identity PODs. */
	bool applyEndpoint();

	/**
	 * @brief Apply shared OSC CLI onto identity/endpoint PODs, then rebind.
	 * @details Recognizes `--network-id`/`--id`, `--listen-port`, `--send-host`,
	 * `--send-port`, `--bind-address` (`=` or following value). Logs the result.
	 * @return false if listen was requested and bind failed.
	 */
	bool applyCommandLine(const CommandLineArgs& args);

	/** @brief One block of CLI lines for app `--help` (no trailing newline). */
	static const char* commandLineHelp();

	/** @brief Send one OSC message using send host/port. */
	bool send(const std::string& address, const std::vector<osc::OscArg>& args);

	/** @brief Prefix, then `networkId`, then leaf - directed at one install. */
	std::string makeAddress(const std::string& leaf) const;

	/** @brief Prefix then leaf - every listener that shares the prefix. */
	std::string makeBroadcastAddress(const std::string& leaf) const;

	bool listening() const;
	const std::string& lastError() const { return m_lastError; }

	/** @brief Self-test: bind, loopback master (directed + broadcast address). */
	bool smokeLoopback();

  private:
	void ensureEntity();
	void handleMessage(const osc::OscMessage& msg);
	std::string leafAddress(const std::string& full) const;
	void maybeSendHeartbeat();
	void noteSender(const osc::OscMessage& msg);

	entt::entity m_entity = entt::null;
	std::unique_ptr<osc::UdpSocket> m_sock;
	std::string m_lastError;
	int m_boundPort = -1;
	std::string m_boundAddress;
	int m_lastSentHeartbeat = -1;
	float m_sendAccum = 0.f;
};

} // namespace rigkit
