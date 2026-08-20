#include "sovd/client/mdns_discovery.hpp"

#ifdef SOVD_HAVE_MDNS

#include <chrono>

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/simple-watch.h>

namespace sovd::client {

namespace {

struct DiscoveryCtx {
    std::vector<DiscoveredServer> *results;
    AvahiClient *client = nullptr;
};

void resolve_callback(AvahiServiceResolver *r, AvahiIfIndex, AvahiProtocol, AvahiResolverEvent event,
                       const char *name, const char *, const char *, const char *host_name,
                       const AvahiAddress *address, uint16_t port, AvahiStringList *, AvahiLookupResultFlags,
                       void *userdata) {
    (void)host_name;
    auto *ctx = static_cast<DiscoveryCtx *>(userdata);
    if (event == AVAHI_RESOLVER_FOUND) {
        char addr_str[AVAHI_ADDRESS_STR_MAX];
        avahi_address_snprint(addr_str, sizeof(addr_str), address);
        // Numeric address, not host_name -- usable by httplib::Client
        // directly with no further DNS/mDNS resolution step needed.
        ctx->results->push_back({name ? name : "", addr_str, port});
    }
    avahi_service_resolver_free(r);
}

void browse_callback(AvahiServiceBrowser *, AvahiIfIndex interface, AvahiProtocol protocol,
                      AvahiBrowserEvent event, const char *name, const char *type, const char *domain,
                      AvahiLookupResultFlags, void *userdata) {
    auto *ctx = static_cast<DiscoveryCtx *>(userdata);
    if (event == AVAHI_BROWSER_NEW) {
        avahi_service_resolver_new(ctx->client, interface, protocol, name, type, domain, AVAHI_PROTO_UNSPEC,
                                    static_cast<AvahiLookupFlags>(0), resolve_callback, ctx);
    }
}

void client_callback(AvahiClient *, AvahiClientState, void *) {
    // No action needed for a one-shot browse: a mid-browse client failure
    // just means discover_sovd_servers() returns whatever it already found
    // once the timeout below elapses, same as finding nothing.
}

} // namespace

std::vector<DiscoveredServer> discover_sovd_servers(int timeout_ms) {
    std::vector<DiscoveredServer> results;

    AvahiSimplePoll *simple_poll = avahi_simple_poll_new();
    if (!simple_poll) return results;

    DiscoveryCtx ctx{&results, nullptr};
    int error = 0;
    AvahiClient *client = avahi_client_new(avahi_simple_poll_get(simple_poll), static_cast<AvahiClientFlags>(0),
                                            client_callback, &ctx, &error);
    if (!client) {
        avahi_simple_poll_free(simple_poll);
        return results;
    }
    ctx.client = client;

    AvahiServiceBrowser *browser =
        avahi_service_browser_new(client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, "_sovd._tcp", nullptr,
                                   static_cast<AvahiLookupFlags>(0), browse_callback, &ctx);
    if (browser) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            avahi_simple_poll_iterate(simple_poll, 100);
        }
        avahi_service_browser_free(browser);
    }

    avahi_client_free(client);
    avahi_simple_poll_free(simple_poll);
    return results;
}

} // namespace sovd::client

#endif // SOVD_HAVE_MDNS
