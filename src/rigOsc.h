#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "OscCodec.h"
#include "core/pack/IPack.h"
#include "core/util/CommandLineArgs.h"
#include "ecs/MEcs.h"

namespace rigkit {

namespace ecs {
struct CNetworkIdentity;
struct COscEndpoint;
} // namespace ecs

namespace osc {
class UdpSocket;
}

/**
 * @brief UDP OSC transport pack.
 * @details Owns sockets. PODs hold network identity and the UDP endpoint.
 * Show-bus addresses are `/<appName>/<networkId>/<leaf>` and broadcast
 * `/<appName>/<leaf>`. The app names each leaf and registers it with
 * @ref addBusListener. Empty `addressPrefix` becomes `/` plus the host app name.
 *
 * Pattern listeners (`?` `*` `[]` `{}`, `**`) share the same socket.
 *
 * After pack setup, product apps call @ref applyCommandLine so network id / ports
 * come from the same CLI everywhere.
 */
class rigOsc : public IPack {
  public:
	using OscListener = std::function<void(const osc::OscRx&)>;

	rigOsc();
	~rigOsc() override;

	bool init() override;
	void setup() override;
	void update(float deltaTime) override;
	void cleanup() override;

	entt::entity configEntity() const { return m_entity; }

	ecs::CNetworkIdentity& identity();
	ecs::COscEndpoint& endpoint();

	/**
	 * @brief Rebind listen socket from current endpoint/identity PODs.
	 * @details A failed bind is sticky for that port/address so `update` does
	 * not retry (and log) every frame. Pass `force` after the user changes the
	 * port.
	 */
	bool applyEndpoint(bool force = false);

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

	/** @brief Send one OSC message to an explicit UDP destination. */
	bool sendTo(const std::string& host, int port, const std::string& address,
				const std::vector<osc::OscArg>& args);

	/** @brief `/<appName>` segment used in show-bus addresses. */
	std::string busPrefix() const;

	/** @brief `/<appName>/<networkId>/<leaf>`. */
	std::string makeAddress(const std::string& leaf) const;

	/** @brief `/<appName>/<leaf>` - no network id. */
	std::string makeBroadcastAddress(const std::string& leaf) const;

	/**
	 * @brief Network id in a directed bus address.
	 * @return Empty when `address` is broadcast or not under @ref busPrefix.
	 */
	std::string busSenderId(const std::string& address) const;

	/**
	 * @brief Register an address-pattern callback. Returns an id for @ref removeListener.
	 * @details Pattern uses OSC 1.0 `?` `*` `[]` `{}` plus pack `**`. No UI here.
	 */
	int addListener(const std::string& pattern, OscListener fn);

	/**
	 * @brief Register one show-bus leaf. Returns an id for @ref removeListener.
	 * @details Fires for `/<appName>/<anyNetworkId>/<leaf>` and `/<appName>/<leaf>`.
	 * The app owns the leaf name and what the arguments mean.
	 */
	int addBusListener(const std::string& leaf, OscListener fn);
	void removeListener(int id);

	bool listening() const;
	int boundPort() const { return m_boundPort; }
	const std::string& lastError() const { return m_lastError; }
	bool hasLastReceived() const { return m_haveRx; }
	const osc::OscRx& lastReceived() const { return m_lastRx; }
	int ignoredPackets() const { return m_ignoredPackets; }

	/** @brief Self-test: bind, loopback a registered leaf (directed + broadcast). */
	bool smokeLoopback();

  private:
	struct Listener {
		int id = 0;
		std::string pattern;
		std::string leaf;
		OscListener fn;
	};

	void ensureEntity();
	void dispatch(const osc::OscRx& rx);
	bool busLeafMatches(const std::string& leaf, const std::string& address) const;
	bool encodeAndSend(const std::string& host, int port, const std::string& address,
					   const std::vector<osc::OscArg>& args);

	entt::entity m_entity = entt::null;
	std::unique_ptr<osc::UdpSocket> m_sock;
	std::string m_lastError;
	int m_boundPort = -1;
	std::string m_boundAddress;
	std::vector<uint8_t> m_rxBuf;
	std::vector<uint8_t> m_txBuf;
	std::vector<Listener> m_listeners;
	int m_nextListenerId = 1;
	osc::OscRx m_lastRx;
	bool m_haveRx = false;
	int m_ignoredPackets = 0;
	bool m_bindFailed = false;
	int m_bindFailPort = -1;
	std::string m_bindFailAddress;
};

} // namespace rigkit
