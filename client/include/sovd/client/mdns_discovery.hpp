// Phase 5: mDNS discovery of SOVD servers advertising `_sovd._tcp.local`
// (docs/DESIGN.md's client config schema: `discovery: mdns`). Only declared/built
// when SOVD_HAVE_MDNS is defined (needs avahi-client, see CMakeLists.txt) --
// callers gate on that macro, matching this project's existing pattern for
// optional compiled-in capability (SOVD_HAVE_UDS_DOIP).
#pragma once

#ifdef SOVD_HAVE_MDNS

#include <cstdint>
#include <string>
#include <vector>

namespace sovd::client {

struct DiscoveredServer {
    std::string name; // service instance name, e.g. "sovd-body-domain"
    std::string host;
    uint16_t port = 0;
};

// Browses _sovd._tcp on the local network for up to timeout_ms, resolving
// every instance found. Blocking, one-shot -- not a live-updating watch.
std::vector<DiscoveredServer> discover_sovd_servers(int timeout_ms = 2000);

} // namespace sovd::client

#endif // SOVD_HAVE_MDNS
