// Phase 5: advertises this sovd_server as `_sovd._tcp.local`, the other
// half of the client SDK's discover_sovd_servers() (client/include/sovd/
// client/mdns_discovery.hpp). Only declared/built when SOVD_HAVE_MDNS is
// defined (needs avahi-client), matching the SOVD_HAVE_UDS_DOIP pattern.
#pragma once

#ifdef SOVD_HAVE_MDNS

#include <cstdint>
#include <string>

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/thread-watch.h>

namespace sovd::server {

// RAII: advertises for the lifetime of the object. Runs its own
// AvahiThreadedPoll (its own background thread), so this doesn't compete
// with httplib::Server's accept loop or block main().
class MdnsAdvertiser {
public:
    MdnsAdvertiser(std::string service_name, uint16_t port);
    ~MdnsAdvertiser();

    MdnsAdvertiser(const MdnsAdvertiser &) = delete;
    MdnsAdvertiser &operator=(const MdnsAdvertiser &) = delete;

private:
    static void client_callback(AvahiClient *c, AvahiClientState state, void *userdata);
    static void entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state, void *userdata);
    void create_services(AvahiClient *c);

    std::string service_name_;
    uint16_t port_;
    AvahiThreadedPoll *poll_ = nullptr;
    AvahiClient *client_ = nullptr;
    AvahiEntryGroup *group_ = nullptr;
};

} // namespace sovd::server

#endif // SOVD_HAVE_MDNS
