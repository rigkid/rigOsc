#include "UdpSocket.h"
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rigkit {
namespace osc {
namespace {

bool ensureWsa() {
#ifdef _WIN32
	static bool once = false;
	static bool ok = false;
	if (!once) {
		once = true;
		WSADATA wsa{};
		ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
	}
	return ok;
#else
	return true;
#endif
}

} // namespace

UdpSocket::~UdpSocket() {
	close();
}

void UdpSocket::close() {
	if (!m_open) {
		return;
	}
#ifdef _WIN32
	closesocket(static_cast<SOCKET>(m_sock));
	m_sock = static_cast<uintptr_t>(INVALID_SOCKET);
#else
	::close(m_sock);
	m_sock = -1;
#endif
	m_open = false;
	m_port = 0;
}

bool UdpSocket::bind(const std::string& address, int port) {
	close();
	m_error.clear();
	if (!ensureWsa()) {
		m_error = "WSAStartup failed";
		return false;
	}
	if (port < 0 || port > 65535) {
		m_error = "invalid port";
		return false;
	}

#ifdef _WIN32
	SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) {
		m_error = "socket() failed";
		return false;
	}
	u_long nonblock = 1;
	ioctlsocket(s, FIONBIO, &nonblock);
	// Exclusive bind — SO_REUSEADDR on Windows UDP lets a second process "bind"
	// the same port, then loopback / peer datagrams land on the first socket.
	BOOL exclusive = 1;
	setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
			   sizeof(exclusive));
#else
	int s = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		m_error = "socket() failed";
		return false;
	}
	int flags = fcntl(s, F_GETFL, 0);
	fcntl(s, F_SETFL, flags | O_NONBLOCK);
	// No SO_REUSEADDR — second listener on the same port must fail with EADDRINUSE.
#endif

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<uint16_t>(port));
	if (address.empty() || address == "0.0.0.0") {
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
	} else if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
		m_error = "inet_pton bind address failed";
#ifdef _WIN32
		closesocket(s);
#else
		::close(s);
#endif
		return false;
	}

	if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
		const int err = WSAGetLastError();
		m_error = (err == WSAEADDRINUSE) ? "bind() failed — port in use"
										 : "bind() failed";
		closesocket(s);
#else
		m_error = (errno == EADDRINUSE) ? "bind() failed — port in use" : "bind() failed";
		::close(s);
#endif
		return false;
	}

	int boundPort = port;
	if (boundPort == 0) {
		sockaddr_in bound{};
#ifdef _WIN32
		int boundLen = sizeof(bound);
		if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
			boundPort = ntohs(bound.sin_port);
		}
#else
		socklen_t boundLen = sizeof(bound);
		if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
			boundPort = ntohs(bound.sin_port);
		}
#endif
	}

#ifdef _WIN32
	m_sock = static_cast<uintptr_t>(s);
#else
	m_sock = s;
#endif
	m_port = boundPort;
	m_open = true;
	return true;
}

bool UdpSocket::sendTo(const std::string& host, int port, const uint8_t* data, size_t len) {
	if (!m_open || !data || len == 0 || port <= 0) {
		return false;
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<uint16_t>(port));
	if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
		m_error = "inet_pton send host failed";
		return false;
	}
#ifdef _WIN32
	const int n = ::sendto(static_cast<SOCKET>(m_sock), reinterpret_cast<const char*>(data),
						   static_cast<int>(len), 0, reinterpret_cast<sockaddr*>(&addr),
						   sizeof(addr));
	return n == static_cast<int>(len);
#else
	const ssize_t n = ::sendto(m_sock, data, len, 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
	return n == static_cast<ssize_t>(len);
#endif
}

size_t UdpSocket::recvFrom(std::vector<uint8_t>& out, std::string* fromHost, int* fromPort) {
	if (!m_open) {
		return 0;
	}
	uint8_t buf[2048];
	sockaddr_in addr{};
#ifdef _WIN32
	int addrLen = sizeof(addr);
	const int n = ::recvfrom(static_cast<SOCKET>(m_sock), reinterpret_cast<char*>(buf), sizeof(buf),
							 0, reinterpret_cast<sockaddr*>(&addr), &addrLen);
	if (n == SOCKET_ERROR) {
		const int err = WSAGetLastError();
		if (err != WSAEWOULDBLOCK) {
			m_error = "recvfrom failed";
		}
		return 0;
	}
#else
	socklen_t addrLen = sizeof(addr);
	const ssize_t n =
		::recvfrom(m_sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&addr), &addrLen);
	if (n < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			m_error = "recvfrom failed";
		}
		return 0;
	}
#endif
	out.assign(buf, buf + n);
	if (fromHost) {
		char ip[INET_ADDRSTRLEN] = {};
		::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
		*fromHost = ip;
	}
	if (fromPort) {
		*fromPort = ntohs(addr.sin_port);
	}
	return static_cast<size_t>(n);
}

} // namespace osc
} // namespace rigkit
