#include "DatagramChannel.hpp"

namespace ucd::screen {

bool DatagramChannel::open(const uconnect::DevId& peer, uconnect::DatagramFallback fallback) {
    if (!topic_) return false;
    if (!topic_->open_datagrams(peer, fallback)) {
        // The other node can negotiate its channel before our worker handles
        // scr.started. Adopt that channel only when the service authorizes open;
        // arrival alone never adds the peer to active_.
        const auto path = topic_->datagram_path(peer);
        if (path == uconnect::DatagramPath::None || path == uconnect::DatagramPath::Failed) return false;
    }
    active_.insert(peer);
    return true;
}

void DatagramChannel::close(const uconnect::DevId& peer) {
    active_.erase(peer);
    if (topic_) topic_->close_datagrams(peer);
}

bool DatagramChannel::send(const uconnect::DevId& peer, std::span<const uint8_t> bytes) {
    if (!active(peer) || bytes.empty() || bytes.size() > uconnect::Topic::max_datagram()) return false;
    if (bytes.front() < 1 || bytes.front() > 5) return false;  // screen 1-2, camera 3, display 4-5
    return topic_ && topic_->send_datagram(peer, bytes);
}

bool DatagramChannel::receive(const uconnect::DevId& peer, std::span<const uint8_t> bytes) {
    if (!active(peer) || bytes.empty() || bytes.size() > uconnect::Topic::max_datagram()) return false;
    if (bytes.front() < 1 || bytes.front() > 5) return false;  // screen 1-2, camera 3, display 4-5
    receive_(peer, bytes.front(), bytes);
    return true;
}

uconnect::DatagramPath DatagramChannel::path(const uconnect::DevId& peer) const {
    return topic_ && active(peer) ? topic_->datagram_path(peer) : uconnect::DatagramPath::None;
}

}  // namespace ucd::screen
