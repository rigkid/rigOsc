#include "TcpSocket.h"

#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#endif

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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rigkit {
namespace osc {
namespace {

constexpr uint8_t kSlipEnd = 0xC0;
constexpr uint8_t kSlipEsc = 0xDB;
constexpr uint8_t kSlipEscEnd = 0xDC;
constexpr uint8_t kSlipEscEsc = 0xDD;

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

void closeSock(uintptr_t sock) {
#ifdef _WIN32
	if (sock != static_cast<uintptr_t>(INVALID_SOCKET)) {
		closesocket(static_cast<SOCKET>(sock));
	}
#else
	if (static_cast<int>(sock) >= 0) {
		::close(static_cast<int>(sock));
	}
#endif
}

bool wouldBlock() {
#ifdef _WIN32
	const int err = WSAGetLastError();
	return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
#endif
}

uint32_t readBe32(const uint8_t* p) {
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void frameSize(const uint8_t* data, size_t len, std::vector<uint8_t>& out) {
	const uint32_t n = static_cast<uint32_t>(len);
	out.push_back(static_cast<uint8_t>((n >> 24) & 0xff));
	out.push_back(static_cast<uint8_t>((n >> 16) & 0xff));
	out.push_back(static_cast<uint8_t>((n >> 8) & 0xff));
	out.push_back(static_cast<uint8_t>(n & 0xff));
	out.insert(out.end(), data, data + len);
}

void frameSlip(const uint8_t* data, size_t len, std::vector<uint8_t>& out) {
	out.push_back(kSlipEnd);
	for (size_t i = 0; i < len; ++i) {
		const uint8_t b = data[i];
		if (b == kSlipEnd) {
			out.push_back(kSlipEsc);
			out.push_back(kSlipEscEnd);
		} else if (b == kSlipEsc) {
			out.push_back(kSlipEsc);
			out.push_back(kSlipEscEsc);
		} else {
			out.push_back(b);
		}
	}
	out.push_back(kSlipEnd);
}

} // namespace

bool waitSockets(const std::vector<SockInterest>& socks, int timeoutMs) {
	if (socks.empty()) {
		if (timeoutMs > 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
		}
		return false;
	}
#ifdef _WIN32
	std::vector<WSAPOLLFD> fds(socks.size());
	for (size_t i = 0; i < socks.size(); ++i) {
		fds[i].fd = static_cast<SOCKET>(socks[i].handle);
		fds[i].events = 0;
		if (socks[i].read) {
			fds[i].events |= POLLIN;
		}
		if (socks[i].write) {
			fds[i].events |= POLLOUT;
		}
		fds[i].revents = 0;
	}
	return WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeoutMs) > 0;
#else
	std::vector<pollfd> fds(socks.size());
	for (size_t i = 0; i < socks.size(); ++i) {
		fds[i].fd = static_cast<int>(socks[i].handle);
		fds[i].events = 0;
		if (socks[i].read) {
			fds[i].events |= POLLIN;
		}
		if (socks[i].write) {
			fds[i].events |= POLLOUT;
		}
		fds[i].revents = 0;
	}
	return ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs) > 0;
#endif
}

TcpPort::~TcpPort() {
	close();
}

void TcpPort::interests(std::vector<SockInterest>& out) const {
	auto add = [&](uintptr_t handle, bool read, bool write) {
		if (handle == static_cast<uintptr_t>(~uintptr_t{0})) {
			return;
		}
		for (SockInterest& row : out) {
			if (row.handle == handle) {
				row.read = row.read || read;
				row.write = row.write || write;
				return;
			}
		}
		out.push_back(SockInterest{handle, read, write});
	};
	if (m_listening) {
		add(m_listen, true, false);
	}
	for (const Conn& c : m_conns) {
		if (c.sock == static_cast<uintptr_t>(~uintptr_t{0})) {
			continue;
		}
		add(c.sock, c.ready, !c.ready || !c.out.empty());
	}
}

void TcpPort::close() {
	for (Conn& c : m_conns) {
		closeSock(c.sock);
	}
	m_conns.clear();
	closeListen();
}

void TcpPort::closeListen() {
	if (m_listening) {
		closeSock(m_listen);
	}
	m_listen = ~uintptr_t{0};
	m_listening = false;
	m_port = 0;
	std::vector<Conn> keep;
	keep.reserve(m_conns.size());
	for (Conn& c : m_conns) {
		if (c.outbound && c.sock != ~uintptr_t{0}) {
			keep.push_back(std::move(c));
		} else {
			closeSock(c.sock);
		}
	}
	m_conns.swap(keep);
}

void TcpPort::setNonBlock(uintptr_t sock) {
#ifdef _WIN32
	u_long nonblock = 1;
	ioctlsocket(static_cast<SOCKET>(sock), FIONBIO, &nonblock);
#else
	int flags = fcntl(static_cast<int>(sock), F_GETFL, 0);
	if (flags >= 0) {
		fcntl(static_cast<int>(sock), F_SETFL, flags | O_NONBLOCK);
	}
#endif
}

void TcpPort::setNoDelay(uintptr_t sock) {
	int on = 1;
	setsockopt(
#ifdef _WIN32
		static_cast<SOCKET>(sock),
#else
		static_cast<int>(sock),
#endif
		IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

bool TcpPort::listen(const std::string& address, int port) {
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
	SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) {
		m_error = "socket() failed";
		return false;
	}
	BOOL reuse = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
	int s = ::socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		m_error = "socket() failed";
		return false;
	}
	int reuse = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
	setNonBlock(static_cast<uintptr_t>(s));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<uint16_t>(port));
	if (address.empty() || address == "0.0.0.0") {
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
	} else if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
		m_error = "inet_pton bind address failed";
		closeSock(static_cast<uintptr_t>(s));
		return false;
	}
	if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
		const int err = WSAGetLastError();
		m_error = (err == WSAEADDRINUSE) ? "tcp bind() failed - port in use" : "tcp bind() failed";
#else
		m_error = (errno == EADDRINUSE) ? "tcp bind() failed - port in use" : "tcp bind() failed";
#endif
		closeSock(static_cast<uintptr_t>(s));
		return false;
	}
	if (::listen(s, SOMAXCONN) != 0) {
		m_error = "listen() failed";
		closeSock(static_cast<uintptr_t>(s));
		return false;
	}

	int boundPort = port;
	if (boundPort == 0) {
		sockaddr_in bound{};
#ifdef _WIN32
		int boundLen = sizeof(bound);
#else
		socklen_t boundLen = sizeof(bound);
#endif
		if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
			boundPort = ntohs(bound.sin_port);
		}
	}
	m_listen = static_cast<uintptr_t>(s);
	m_port = boundPort;
	m_listening = true;
	return true;
}

bool TcpPort::connect(const std::string& host, int port) {
	m_error.clear();
	if (!ensureWsa() || host.empty() || port <= 0 || port > 65535) {
		m_error = "tcp connect failed";
		return false;
	}
	if (find(host, port)) {
		return true;
	}

#ifdef _WIN32
	SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) {
		m_error = "socket() failed";
		return false;
	}
#else
	int s = ::socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		m_error = "socket() failed";
		return false;
	}
#endif
	setNonBlock(static_cast<uintptr_t>(s));
	setNoDelay(static_cast<uintptr_t>(s));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<uint16_t>(port));
	if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
		m_error = "inet_pton send host failed";
		closeSock(static_cast<uintptr_t>(s));
		return false;
	}
	const int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
	if (rc != 0 && !wouldBlock()) {
		m_error = "tcp connect failed";
		closeSock(static_cast<uintptr_t>(s));
		return false;
	}

	Conn c;
	c.sock = static_cast<uintptr_t>(s);
	c.host = host;
	c.port = port;
	c.outbound = true;
	c.ready = (rc == 0);
	m_conns.push_back(std::move(c));
	return true;
}

TcpPort::Conn* TcpPort::find(const std::string& host, int port) {
	Conn* accepted = nullptr;
	for (Conn& c : m_conns) {
		if (c.host == host && c.port == port) {
			if (c.outbound) {
				return &c;
			}
			accepted = &c;
		}
	}
	return accepted;
}

const TcpPort::Conn* TcpPort::find(const std::string& host, int port) const {
	const Conn* accepted = nullptr;
	for (const Conn& c : m_conns) {
		if (c.host == host && c.port == port) {
			if (c.outbound) {
				return &c;
			}
			accepted = &c;
		}
	}
	return accepted;
}

bool TcpPort::hasPeer(const std::string& host, int port) const {
	return find(host, port) != nullptr;
}

void TcpPort::drop(Conn& c) {
	closeSock(c.sock);
	c.sock = ~uintptr_t{0};
}

bool TcpPort::flush(Conn& c) {
	while (!c.out.empty()) {
		size_t chunk = c.out.size();
		if (chunk > 0x7fffffffu) {
			chunk = 0x7fffffffu;
		}
#ifdef _WIN32
		const int n =
			::send(static_cast<SOCKET>(c.sock), reinterpret_cast<const char*>(c.out.data()),
				   static_cast<int>(chunk), 0);
#else
		const ssize_t n = ::send(static_cast<int>(c.sock), c.out.data(), chunk, MSG_NOSIGNAL);
#endif
		if (n > 0) {
			c.out.erase(c.out.begin(), c.out.begin() + static_cast<std::ptrdiff_t>(n));
			continue;
		}
		if (wouldBlock()) {
			return true;
		}
		drop(c);
		++m_discarded;
		return false;
	}
	return true;
}

bool TcpPort::sendTo(const std::string& host, int port, const uint8_t* data, size_t len,
					 int framing) {
	Conn* c = find(host, port);
	if (!c || !data || len == 0) {
		return false;
	}
	if (framing == kFramingSlip) {
		frameSlip(data, len, c->out);
	} else if (len > 0xffffffffu) {
		m_error = "tcp size prefix is 32-bit";
		return false;
	} else {
		frameSize(data, len, c->out);
	}
	if (!c->ready) {
		return true;
	}
	return flush(*c);
}

void TcpPort::takeSize(Conn& c, std::vector<TcpPacket>& out) {
	while (c.acc.size() >= 4) {
		const size_t n = readBe32(c.acc.data());
		if (n == 0) {
			c.acc.erase(c.acc.begin(), c.acc.begin() + 4);
			continue;
		}
		if (c.acc.size() - 4 < n) {
			return;
		}
		TcpPacket pkt;
		pkt.bytes.assign(c.acc.begin() + 4, c.acc.begin() + 4 + static_cast<std::ptrdiff_t>(n));
		pkt.fromHost = c.host;
		pkt.fromPort = c.port;
		out.push_back(std::move(pkt));
		c.acc.erase(c.acc.begin(), c.acc.begin() + 4 + static_cast<std::ptrdiff_t>(n));
	}
}

void TcpPort::takeSlip(Conn& c, std::vector<TcpPacket>& out) {
	size_t consumed = 0;
	for (size_t i = 0; i < c.acc.size(); ++i) {
		const uint8_t b = c.acc[i];
		consumed = i + 1;
		if (b == kSlipEnd) {
			if (c.slipOpen && !c.slip.empty()) {
				TcpPacket pkt;
				pkt.bytes = std::move(c.slip);
				pkt.fromHost = c.host;
				pkt.fromPort = c.port;
				out.push_back(std::move(pkt));
			}
			c.slip.clear();
			c.slipEsc = false;
			c.slipOpen = true;
			continue;
		}
		if (!c.slipOpen) {
			continue;
		}
		if (c.slipEsc) {
			c.slipEsc = false;
			uint8_t decoded = 0;
			if (b == kSlipEscEnd) {
				decoded = kSlipEnd;
			} else if (b == kSlipEscEsc) {
				decoded = kSlipEsc;
			} else {
				drop(c);
				++m_discarded;
				c.acc.clear();
				return;
			}
			c.slip.push_back(decoded);
		} else if (b == kSlipEsc) {
			c.slipEsc = true;
		} else {
			c.slip.push_back(b);
		}
	}
	if (consumed > 0) {
		c.acc.erase(c.acc.begin(), c.acc.begin() + static_cast<std::ptrdiff_t>(consumed));
	}
}

void TcpPort::poll(int framing, std::vector<TcpPacket>& out) {
	if (m_listening) {
		while (true) {
			sockaddr_in peer{};
#ifdef _WIN32
			int peerLen = sizeof(peer);
			SOCKET accepted = ::accept(static_cast<SOCKET>(m_listen),
									   reinterpret_cast<sockaddr*>(&peer), &peerLen);
			if (accepted == INVALID_SOCKET) {
				break;
			}
#else
			socklen_t peerLen = sizeof(peer);
			int accepted =
				::accept(static_cast<int>(m_listen), reinterpret_cast<sockaddr*>(&peer), &peerLen);
			if (accepted < 0) {
				break;
			}
#endif
			setNonBlock(static_cast<uintptr_t>(accepted));
			setNoDelay(static_cast<uintptr_t>(accepted));
			Conn c;
			c.sock = static_cast<uintptr_t>(accepted);
			c.ready = true;
			char ip[INET_ADDRSTRLEN] = {};
			::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
			c.host = ip;
			c.port = ntohs(peer.sin_port);
			m_conns.push_back(std::move(c));
		}
	}

	for (Conn& c : m_conns) {
		if (c.sock == ~uintptr_t{0} || c.ready) {
			continue;
		}
		fd_set writefds;
		FD_ZERO(&writefds);
#ifdef _WIN32
		FD_SET(static_cast<SOCKET>(c.sock), &writefds);
		timeval tv{};
		const int sel = ::select(0, nullptr, &writefds, nullptr, &tv);
#else
		FD_SET(static_cast<int>(c.sock), &writefds);
		timeval tv{};
		const int sel = ::select(static_cast<int>(c.sock) + 1, nullptr, &writefds, nullptr, &tv);
#endif
		if (sel <= 0) {
			continue;
		}
		int soerr = 0;
#ifdef _WIN32
		int soerrLen = sizeof(soerr);
		getsockopt(static_cast<SOCKET>(c.sock), SOL_SOCKET, SO_ERROR,
				   reinterpret_cast<char*>(&soerr), &soerrLen);
#else
		socklen_t soerrLen = sizeof(soerr);
		getsockopt(static_cast<int>(c.sock), SOL_SOCKET, SO_ERROR, &soerr, &soerrLen);
#endif
		if (soerr == 0) {
			c.ready = true;
		} else {
			drop(c);
			++m_discarded;
		}
	}

	for (Conn& c : m_conns) {
		if (c.sock != ~uintptr_t{0} && c.ready && !c.out.empty()) {
			flush(c);
		}
	}

	uint8_t buf[4096];
	for (Conn& c : m_conns) {
		if (c.sock == ~uintptr_t{0} || !c.ready) {
			continue;
		}
		while (true) {
#ifdef _WIN32
			const int n =
				::recv(static_cast<SOCKET>(c.sock), reinterpret_cast<char*>(buf), sizeof(buf), 0);
#else
			const ssize_t n = ::recv(static_cast<int>(c.sock), buf, sizeof(buf), 0);
#endif
			if (n > 0) {
				c.acc.insert(c.acc.end(), buf, buf + n);
				if (framing == kFramingSlip) {
					takeSlip(c, out);
				} else {
					takeSize(c, out);
				}
				if (c.sock == ~uintptr_t{0}) {
					break;
				}
				continue;
			}
			if (n == 0) {
				drop(c);
				break;
			}
			if (!wouldBlock()) {
				drop(c);
				++m_discarded;
			}
			break;
		}
	}

	std::vector<Conn> live;
	live.reserve(m_conns.size());
	for (Conn& c : m_conns) {
		if (c.sock != ~uintptr_t{0}) {
			live.push_back(std::move(c));
		}
	}
	m_conns.swap(live);
}

} // namespace osc
} // namespace rigkit
