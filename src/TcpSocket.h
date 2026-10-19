#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rigkit {
namespace osc {

/** @brief One socket the pack thread should wait on. */
struct SockInterest {
	uintptr_t handle = 0;
	bool read = false;
	bool write = false;
};

/** @brief Wait until any socket is ready, or `timeoutMs` elapses. */
bool waitSockets(const std::vector<SockInterest>& socks, int timeoutMs);

/** @brief One stream-framed OSC packet plus the peer that sent it. */
struct TcpPacket {
	std::vector<uint8_t> bytes;
	std::string fromHost;
	int fromPort = 0;
};

/**
 * @brief Non-blocking TCP listen and client sockets.
 * @details Framing: `0` is a 4-byte big-endian size (the show-control default),
 * `1` is SLIP (`END` `0xC0`, `ESC` `0xDB`). The pack calls this from its socket thread.
 * How many peers, and how large a packet, is the host's choice.
 */
class TcpPort {
  public:
	static constexpr int kFramingSize = 0;
	static constexpr int kFramingSlip = 1;

	TcpPort() = default;
	~TcpPort();

	TcpPort(const TcpPort&) = delete;
	TcpPort& operator=(const TcpPort&) = delete;

	bool listen(const std::string& address, int port);
	void close();
	/** @brief Close the listen socket and accepted clients. Dialed peers stay. */
	void closeListen();
	bool isListening() const { return m_listening; }
	int port() const { return m_port; }

	/** @brief Dial `host:port`. Returns false when the socket cannot be opened. */
	bool connect(const std::string& host, int port);

	/** @brief True when a client socket is aimed at `host:port`. */
	bool hasPeer(const std::string& host, int port) const;

	/**
	 * @brief Frame `data` and queue it on the matching client.
	 * @return False when no client matches.
	 */
	bool sendTo(const std::string& host, int port, const uint8_t* data, size_t len, int framing);

	/**
	 * @brief Accept, finish dials, and pull every complete packet already buffered.
	 * @param framing `kFramingSize` or `kFramingSlip`.
	 */
	void poll(int framing, std::vector<TcpPacket>& out);

	int discarded() const { return m_discarded; }
	const std::string& lastError() const { return m_error; }

	/** @brief Listen and client sockets the pack thread should wait on. */
	void interests(std::vector<SockInterest>& out) const;

  private:
	struct Conn {
		uintptr_t sock = ~uintptr_t{0};
		std::string host;
		int port = 0;
		bool outbound = false;
		bool ready = false;
		std::vector<uint8_t> acc;
		std::vector<uint8_t> out;
		bool slipOpen = false;
		bool slipEsc = false;
		std::vector<uint8_t> slip;
	};

	Conn* find(const std::string& host, int port);
	const Conn* find(const std::string& host, int port) const;
	void drop(Conn& c);
	bool flush(Conn& c);
	void takeSize(Conn& c, std::vector<TcpPacket>& out);
	void takeSlip(Conn& c, std::vector<TcpPacket>& out);
	void setNonBlock(uintptr_t sock);
	void setNoDelay(uintptr_t sock);

	bool m_listening = false;
	int m_port = 0;
	int m_discarded = 0;
	std::string m_error;
	uintptr_t m_listen = ~uintptr_t{0};
	std::vector<Conn> m_conns;
};

} // namespace osc
} // namespace rigkit
