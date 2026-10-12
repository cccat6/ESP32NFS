// SPDX-License-Identifier: MIT
#pragma once
#include "../ESP32NFS.h"
#include <WiFi.h>
#include <algorithm>

namespace esp32_nfs {
// Arduino-Pico uses a raw-lwIP WiFiClient API. Capacity-bounded writes copy into
// its send queue; zero stream timeout and asynchronous mode keep polling short.
class PicoWiFiTransport : public Transport {
public:
  ~PicoWiFiTransport() override {
    stop();
  }
  PicoWiFiTransport() = default;
  PicoWiFiTransport(const PicoWiFiTransport &) = delete;
  PicoWiFiTransport &operator=(const PicoWiFiTransport &) = delete;
  void enableDiscovery(bool enabled) {
    discovery_ = enabled;
  }
  void setNoDelay(bool enabled) {
    noDelay_ = enabled;
  }
  bool listen(uint16_t port) override {
    stop();
    listener_.begin(port, ESP32_NFS_CONNECTION_COUNT);
    if (!listener_)
      return false;
    if (discovery_ && port != 111) {
      mapper_.begin(111, ESP32_NFS_CONNECTION_COUNT);
      if (!mapper_) {
        stop();
        return false;
      }
    }
    return true;
  }
  int accept(uint64_t &peer) override {
    WiFiClient incoming = listener_.accept();
    if (!incoming && mapper_)
      incoming = mapper_.accept();
    if (!incoming)
      return -1;
    for (unsigned i = 0; i < ESP32_NFS_CONNECTION_COUNT; ++i) {
      if (used_[i])
        continue;
      incoming.setTimeout(0);
      incoming.setSync(false);
      incoming.setNoDelay(noDelay_);
      auto ip = incoming.remoteIP();
      peer = (uint32_t(ip[0]) << 24) | (uint32_t(ip[1]) << 16) | (uint32_t(ip[2]) << 8) |
             uint32_t(ip[3]);
      clients_[i] = incoming;
      used_[i] = true;
      return int(i);
    }
    incoming.stop(1);
    return -1;
  }
  bool connected(int socket) override {
    return valid(socket) && (clients_[socket].connected() || clients_[socket].available() > 0);
  }
  int available(int socket) override {
    return valid(socket) ? std::max(0, clients_[socket].available()) : 0;
  }
  int read(int socket, uint8_t *data, size_t size) override {
    if (!valid(socket))
      return -1;
    int ready = available(socket);
    if (!ready)
      return connected(socket) ? 0 : -1;
    return clients_[socket].read(data, std::min(size, size_t(ready)));
  }
  int write(int socket, const uint8_t *data, size_t size) override {
    if (!connected(socket))
      return -1;
    int space = clients_[socket].availableForWrite();
    if (space <= 0)
      return 0;
    size_t count = std::min(size, size_t(space));
    size_t written = clients_[socket].write(data, count);
    return written <= count ? int(written) : -1;
  }
  void close(int socket) override {
    if (valid(socket)) {
      // The Pico API interprets zero as its default flush wait; one bounds teardown.
      clients_[socket].stop(1);
      clients_[socket] = WiFiClient();
      used_[socket] = false;
    }
  }
  void stop() override {
    for (unsigned i = 0; i < ESP32_NFS_CONNECTION_COUNT; ++i)
      close(i);
    listener_.end();
    mapper_.end();
  }

private:
  WiFiServer listener_, mapper_;
  WiFiClient clients_[ESP32_NFS_CONNECTION_COUNT];
  bool used_[ESP32_NFS_CONNECTION_COUNT] = {};
  bool discovery_ = false, noDelay_ = true;
  bool valid(int socket) const {
    return socket >= 0 && unsigned(socket) < ESP32_NFS_CONNECTION_COUNT && used_[socket];
  }
};
} // namespace esp32_nfs
