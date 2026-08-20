#include "sovd/server/mdns_advertise.hpp"

#ifdef SOVD_HAVE_MDNS

#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>

namespace sovd::server {

void MdnsAdvertiser::create_services(AvahiClient *c) {
    if (!group_) {
        group_ = avahi_entry_group_new(c, entry_group_callback, this);
        if (!group_) return;
    }
    if (!avahi_entry_group_is_empty(group_)) return;

    int ret = avahi_entry_group_add_service(group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                                             static_cast<AvahiPublishFlags>(0), service_name_.c_str(), "_sovd._tcp",
                                             nullptr, nullptr, port_, nullptr);
    if (ret == AVAHI_ERR_COLLISION) {
        // Another instance on the network already used this name -- pick
        // the next alternative (matches every avahi-client example's
        // documented collision handling) rather than failing to advertise
        // at all.
        char *alt = avahi_alternative_service_name(service_name_.c_str());
        service_name_ = alt;
        avahi_free(alt);
        avahi_entry_group_reset(group_);
        create_services(c);
        return;
    }
    if (ret < 0) return;
    avahi_entry_group_commit(group_);
}

void MdnsAdvertiser::entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state, void *userdata) {
    auto *self = static_cast<MdnsAdvertiser *>(userdata);
    if (state == AVAHI_ENTRY_GROUP_COLLISION) {
        char *alt = avahi_alternative_service_name(self->service_name_.c_str());
        self->service_name_ = alt;
        avahi_free(alt);
        self->create_services(avahi_entry_group_get_client(g));
    }
    // ESTABLISHED / FAILURE / UNCOMMITED / REGISTERING: nothing else to do
    // here -- a failure just means this process stays undiscoverable,
    // which is the same as mDNS never having been enabled at all.
}

void MdnsAdvertiser::client_callback(AvahiClient *c, AvahiClientState state, void *userdata) {
    auto *self = static_cast<MdnsAdvertiser *>(userdata);
    if (state == AVAHI_CLIENT_S_RUNNING) {
        self->create_services(c);
    } else if (state == AVAHI_CLIENT_S_COLLISION || state == AVAHI_CLIENT_S_REGISTERING) {
        if (self->group_) avahi_entry_group_reset(self->group_);
    }
    // FAILURE / CONNECTING: nothing to do -- same "just stays
    // undiscoverable" reasoning as above.
}

MdnsAdvertiser::MdnsAdvertiser(std::string service_name, uint16_t port)
    : service_name_(std::move(service_name)), port_(port) {
    poll_ = avahi_threaded_poll_new();
    if (!poll_) return;

    int error = 0;
    client_ = avahi_client_new(avahi_threaded_poll_get(poll_), static_cast<AvahiClientFlags>(0), client_callback,
                                this, &error);
    if (!client_) {
        avahi_threaded_poll_free(poll_);
        poll_ = nullptr;
        return;
    }
    avahi_threaded_poll_start(poll_);
}

MdnsAdvertiser::~MdnsAdvertiser() {
    if (poll_) avahi_threaded_poll_stop(poll_);
    if (client_) avahi_client_free(client_); // also frees group_, owned by the client
    if (poll_) avahi_threaded_poll_free(poll_);
}

} // namespace sovd::server

#endif // SOVD_HAVE_MDNS
