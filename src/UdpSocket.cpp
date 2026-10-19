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
#include <sys/select.h>
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

uintptr_t UdpSocket::handle() const {
	if (!m_open) {
		return static_cast<uintptr_t>(~uintptr_t{0});
	}
	return static_cast<uintptr_t>(m_sock);
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

bool UdpSocket::bind(const std::string& address, int port, const std::string& multicastGroup,
					 int multicastTtl) {
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
	if (multicastTtl < 0) {
		multicastTtl = 0;
	}
	if (multicastTtl > 255) {
		multicastTtl = 255;
	}

	in_addr iface{};
	iface.s_addr = htonl(INADDR_ANY);
	const bool any = address.empty() || address == "0.0.0.0";
	if (!any && ::inet_pton(AF_INET, address.c_str(), &iface) != 1) {
		m_error = "inet_pton bind address failed";
		return false;
	}

	in_addr group{};
	const bool join = !multicastGroup.empty();
	if (join) {
		if (::inet_pton(AF_INET, multicastGroup.c_str(), &group) != 1) {
			m_error = "inet_pton multicast group failed";
			return false;
		}
		const uint32_t host = ntohl(group.s_addr);
		if (host < 0xE0000000u || host > 0xEFFFFFFFu) {
			m_error = "not a multicast group";
			return false;
		}
	}

#ifdef _WIN32
	SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) {
		m_error = "socket() failed";
		return false;
	}
	u_long nonblock = 1;
	if (ioctlsocket(s, FIONBIO, &nonblock) == SOCKET_ERROR) {
		m_error = "non-blocking socket failed";
		closesocket(s);
		return false;
	}
	if (join) {
		// Shared port so a second process can join the same group.
		BOOL reuse = 1;
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
				   sizeof(reuse));
	} else {
		// Exclusive bind - SO_REUSEADDR on Windows UDP lets a second process "bind"
		// the same port, then loopback / peer datagrams land on the first socket.
		BOOL exclusive = 1;
		setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
				   sizeof(exclusive));
	}
	BOOL broadcast = 1;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast),
			   sizeof(broadcast));
#else
	int s = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		m_error = "socket() failed";
		return false;
	}
	int flags = fcntl(s, F_GETFL, 0);
	if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) {
		m_error = "non-blocking socket failed";
		::close(s);
		return false;
	}
	if (join) {
		int reuse = 1;
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	}
	// Unicast stays exclusive: no SO_REUSEADDR, so a second listener gets EADDRINUSE.
	int broadcast = 1;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
#endif

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<uint16_t>(port));
	// Multicast is received on the wildcard bind; the group picks the traffic.
	addr.sin_addr.s_addr = join ? htonl(INADDR_ANY) : iface.s_addr;

	if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
		const int err = WSAGetLastError();
		m_error = (err == WSAEADDRINUSE) ? "bind() failed - port in use" : "bind() failed";
		closesocket(s);
#else
		m_error = (errno == EADDRINUSE) ? "bind() failed - port in use" : "bind() failed";
		::close(s);
#endif
		return false;
	}

	if (join) {
		ip_mreq mreq{};
		mreq.imr_multiaddr = group;
		mreq.imr_interface = iface;
		if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq),
					   sizeof(mreq)) != 0) {
			m_error = "multicast join failed";
#ifdef _WIN32
			closesocket(s);
#else
			::close(s);
#endif
			return false;
		}
#ifdef _WIN32
		DWORD ttl = static_cast<DWORD>(multicastTtl);
		DWORD loop = 1;
#else
		int ttl = multicastTtl;
		int loop = 1;
#endif
		setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl),
				   sizeof(ttl));
		setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop),
				   sizeof(loop));
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
	if (m_buf.size() < 65536) {
		m_buf.resize(65536);
	}
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
	const int n =
		::sendto(static_cast<SOCKET>(m_sock), reinterpret_cast<const char*>(data),
				 static_cast<int>(len), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
	return n == static_cast<int>(len);
#else
	const ssize_t n =
		::sendto(m_sock, data, len, 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
	return n == static_cast<ssize_t>(len);
#endif
}

size_t UdpSocket::recvFrom(std::vector<uint8_t>& out, std::string* fromHost, int* fromPort) {
	if (!m_open) {
		return 0;
	}
	if (m_buf.size() < 65536) {
		m_buf.resize(65536);
	}
	sockaddr_in addr{};
#ifdef _WIN32
	SOCKET s = static_cast<SOCKET>(m_sock);
	fd_set readfds;
	FD_ZERO(&readfds);
	FD_SET(s, &readfds);
	timeval tv{};
	if (::select(0, &readfds, nullptr, nullptr, &tv) <= 0) {
		return 0;
	}
	int addrLen = sizeof(addr);
	const int n =
		::recvfrom(s, reinterpret_cast<char*>(m_buf.data()), static_cast<int>(m_buf.size()), 0,
				   reinterpret_cast<sockaddr*>(&addr), &addrLen);
	if (n == SOCKET_ERROR) {
		const int err = WSAGetLastError();
		if (err != WSAEWOULDBLOCK && err != WSAECONNRESET) {
			m_error = "recvfrom failed";
		}
		return 0;
	}
#else
	fd_set readfds;
	FD_ZERO(&readfds);
	FD_SET(m_sock, &readfds);
	timeval tv{};
	if (::select(m_sock + 1, &readfds, nullptr, nullptr, &tv) <= 0) {
		return 0;
	}
	socklen_t addrLen = sizeof(addr);
	const ssize_t n = ::recvfrom(m_sock, m_buf.data(), m_buf.size(), 0,
								 reinterpret_cast<sockaddr*>(&addr), &addrLen);
	if (n < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			m_error = "recvfrom failed";
		}
		return 0;
	}
#endif
	out.assign(m_buf.begin(), m_buf.begin() + n);
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
