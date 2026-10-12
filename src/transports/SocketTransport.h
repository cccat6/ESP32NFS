// SPDX-License-Identifier: MIT
#pragma once
#include "../ESP32NFS.h"
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <lwip/tcp.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <errno.h>
#include <fcntl.h>
namespace esp32_nfs {
// Raw nonblocking sockets avoid NetworkClient's extra heap RX buffer and write retry loop.
class SocketTransport : public Transport {
public:
  explicit SocketTransport(uint32_t bindAddress = INADDR_ANY) : bindAddress_(bindAddress) {}
  ~SocketTransport() {
    stop();
  }
  void enableDiscovery(bool enable) {
    discovery_ = enable;
  } // additional TCP/111 listener
  // Applies to subsequently accepted sockets. Measure batching against latency on the target LAN.
  void setNoDelay(bool enable) {
    noDelay_ = enable;
  }
  bool listen(uint16_t port) override {
    stop();
    listener_ = openListener(port);
    if (listener_ < 0)
      return false;
    if (discovery_ && port != 111) {
      mapper_ = openListener(111);
      if (mapper_ < 0) {
        stop();
        return false;
      }
    }
    return true;
  }
  int accept(uint64_t &peer) override {
    sockaddr_in addr = {};
    socklen_t n = sizeof(addr);
    int fd = ::accept(listener_, reinterpret_cast<sockaddr *>(&addr), &n);
    if (fd < 0 && mapper_ >= 0)
      fd = ::accept(mapper_, reinterpret_cast<sockaddr *>(&addr), &n);
    if (fd < 0)
      return -1;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
      ::close(fd);
      return -1;
    }
    int noDelay = noDelay_ ? 1 : 0;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    peer = ntohl(addr.sin_addr.s_addr);
    return fd;
  }
  bool connected(int fd) override {
    uint8_t byte;
    int n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return n > 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
  }
  int available(int fd) override {
    uint8_t byte;
    return recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) > 0 ? 1 : 0;
  }
  int read(int fd, uint8_t *p, size_t n) override {
    int r = recv(fd, p, n, MSG_DONTWAIT);
    return r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : r;
  }
  int write(int fd, const uint8_t *p, size_t n) override {
    int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    int r = send(fd, p, n, flags);
    return r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : r;
  }
  void close(int fd) override {
    ::close(fd);
  }
  void stop() override {
    if (listener_ >= 0)
      ::close(listener_);
    if (mapper_ >= 0)
      ::close(mapper_);
    listener_ = mapper_ = -1;
  }

private:
  int listener_ = -1, mapper_ = -1;
  uint32_t bindAddress_;
  bool discovery_ = false, noDelay_ = true;
  int openListener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
      return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(bindAddress_);
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 ||
        bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0 ||
        ::listen(fd, ESP32_NFS_CONNECTION_COUNT) < 0) {
      ::close(fd);
      return -1;
    }
    return fd;
  }
};
} // namespace esp32_nfs
