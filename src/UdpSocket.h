#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rigkit {
namespace osc {

/**
 * @brief Non-blocking UDP socket (Winsock / BSD).
 * @details Handles stay here — never in POD components.
 */
class UdpSocket {
  public:
	UdpSocket() = default;
	~UdpSocket();

	UdpSocket(const UdpSocket&) = delete;
	UdpSocket& operator=(const UdpSocket&) = delete;

	bool bind(const std::string& address, int port);
	void close();
	bool isOpen() const { return m_open; }

	/** @brief Send datagram. Returns false on failure. */
	bool sendTo(const std::string& host, int port, const uint8_t* data, size_t len);

	/**
	 * @brief Non-blocking receive into `out`.
	 * @return Bytes received, or 0 if none / error.
	 */
	size_t recvFrom(std::vector<uint8_t>& out, std::string* fromHost = nullptr,
					int* fromPort = nullptr);

	const std::string& lastError() const { return m_error; }
	int port() const { return m_port; }

  private:
	bool m_open = false;
	int m_port = 0;
	std::string m_error;
#ifdef _WIN32
	uintptr_t m_sock = static_cast<uintptr_t>(~0); ///< SOCKET
#else
	int m_sock = -1;
#endif
};

} // namespace osc
} // namespace rigkit
