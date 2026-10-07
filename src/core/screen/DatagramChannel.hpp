#pragma once

#include <functional>
#include <set>
#include "uconnect/uconnect.hpp"

namespace ucd::screen {

// Owned by the Session worker. Opening is a deliberate action after the
// service's trust/consent checks; unsolicited channels never deliver media.
class DatagramChannel {
public:
    using Receive = std::function<void(const uconnect::DevId&, uint8_t, std::span<const uint8_t>)>;
    explicit DatagramChannel(Receive receive) : receive_(std::move(receive)) {}
    void attach(uconnect::Topic* topic) { active_.clear(); topic_ = topic; }
    bool open(const uconnect::DevId& peer, uconnect::DatagramFallback fallback = uconnect::DatagramFallback::Tcp);
    void close(const uconnect::DevId& peer);
    bool send(const uconnect::DevId& peer, std::span<const uint8_t> bytes);
    bool receive(const uconnect::DevId& peer, std::span<const uint8_t> bytes);
    bool active(const uconnect::DevId& peer) const { return active_.contains(peer); }
    uconnect::DatagramPath path(const uconnect::DevId& peer) const;
private:
    uconnect::Topic* topic_ = nullptr;
    std::set<uconnect::DevId> active_;
    Receive receive_;
};

}  // namespace ucd::screen
