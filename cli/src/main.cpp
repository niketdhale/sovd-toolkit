// Phase 5 CLI. Deliberately zero hardcoded entity/DID knowledge anywhere in
// this file -- every path/id is a runtime argument, and `docs` is how a
// user (or a script) discovers what's actually callable on a given entity.
// That's what "discovery-driven, not hardcoded" (CLAUDE.md) means for a CLI
// specifically: the same constraint Phase 7's web UI restates for a GUI.
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <vector>

#include "sovd/client/lock_guard.hpp"
#include "sovd/client/sovd_client.hpp"

#ifdef SOVD_HAVE_MDNS
#include "sovd/client/mdns_discovery.hpp"
#endif

using namespace sovd::client;

namespace {

std::vector<std::string> split_ids(const std::string &s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    for (std::string token; std::getline(iss, token, ','); ) {
        if (!token.empty()) out.push_back(token);
    }
    return out;
}

void print_data_value(const DataValue &v) {
    if (v.error) {
        std::cout << v.id << ": ERROR " << *v.error << " (" << v.error_message.value_or("") << ")\n";
        return;
    }
    std::cout << v.id;
    if (v.did) std::cout << " [" << *v.did << "]";
    std::cout << " = " << v.value.dump();
    if (v.unit) std::cout << " " << *v.unit;
    std::cout << "\n";
}

void print_docs(const DocsResult &docs) {
    std::cout << "path: " << docs.path << "  type: " << docs.type << "  has_backend: "
              << (docs.has_backend ? "true" : "false") << "\n";
    if (docs.capabilities) {
        std::cout << "capabilities: batch_read=" << docs.capabilities->supports_batch_read
                   << " async_operations=" << docs.capabilities->supports_async_operations
                   << " io_control=" << docs.capabilities->supports_io_control << "\n";
    }
    std::cout << "data:\n";
    for (auto &d : docs.data) {
        std::cout << "  " << d.id << "  did=" << d.did << "  type=" << d.type << "  access=" << d.access;
        if (d.unit) std::cout << "  unit=" << *d.unit;
        if (d.io_control) std::cout << "  io_control";
        std::cout << "\n";
    }
    std::cout << "operations:\n";
    for (auto &o : docs.operations) {
        std::cout << "  " << o.id << "  routine_id=" << o.routine_id << "  async=" << (o.async ? "true" : "false");
        if (o.requires_session) std::cout << "  requires_session=" << *o.requires_session;
        std::cout << "\n";
    }
}

int usage() {
    std::cerr << "usage:\n"
                 "  sovd-cli discover [timeout_ms]\n"
                 "  sovd-cli <base_url> entities\n"
                 "  sovd-cli <base_url> docs <path>\n"
                 "  sovd-cli <base_url> faults <path> [status_filter]\n"
                 "  sovd-cli <base_url> faults-clear <path>\n"
                 "  sovd-cli <base_url> read <path> <id[,id2,...]>\n"
                 "  sovd-cli <base_url> write <path> <id> <hex_value>\n"
                 "  sovd-cli <base_url> mode <path> <mode_name>\n"
                 "  sovd-cli <base_url> op <path> <op_name> [params_json]\n"
                 "  sovd-cli <base_url> watch <path> <id> [interval_ms]\n";
    return 1;
}

std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop.store(true); }

int run_discover(int argc, char **argv) {
#ifdef SOVD_HAVE_MDNS
    int timeout_ms = argc > 2 ? std::atoi(argv[2]) : 2000;
    auto servers = discover_sovd_servers(timeout_ms);
    if (servers.empty()) {
        std::cout << "no _sovd._tcp servers found in " << timeout_ms << "ms\n";
        return 0;
    }
    for (auto &s : servers) {
        std::cout << s.name << "  http://" << s.host << ":" << s.port << "\n";
    }
    return 0;
#else
    (void)argc;
    (void)argv;
    std::cerr << "mDNS discovery not compiled into this binary (needs avahi-client; see CMakeLists.txt "
                 "SOVD_CLIENT_MDNS)\n";
    return 1;
#endif
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    if (std::string(argv[1]) == "discover") return run_discover(argc, argv);
    if (argc < 3) return usage();

    std::string base_url = argv[1];
    std::string command = argv[2];

    try {
        SovdClient cli(base_url);

        if (command == "entities") {
            for (auto &e : cli.list_entities()) {
                std::cout << e.path << "  type=" << e.type << "  has_backend=" << (e.has_backend ? "true" : "false")
                          << "\n";
            }
            return 0;
        }

        if (command == "docs") {
            if (argc < 4) return usage();
            print_docs(cli.get_docs(argv[3]));
            return 0;
        }

        if (command == "faults") {
            if (argc < 4) return usage();
            std::string status_filter = argc > 4 ? argv[4] : "";
            for (auto &f : cli.get_faults(argv[3], status_filter)) {
                std::cout << f.code << "  " << f.status << "\n";
            }
            return 0;
        }

        if (command == "faults-clear") {
            if (argc < 4) return usage();
            LockGuard lock(cli, argv[3], 60, /*heartbeat=*/false);
            cli.clear_faults(argv[3], lock.lock_id());
            std::cout << "cleared\n";
            return 0;
        }

        if (command == "read") {
            if (argc < 5) return usage();
            auto ids = split_ids(argv[4]);
            if (ids.size() == 1) {
                print_data_value(cli.get_data(argv[3], ids[0]));
            } else {
                for (auto &v : cli.get_data_batch(argv[3], ids)) print_data_value(v);
            }
            return 0;
        }

        if (command == "write") {
            if (argc < 6) return usage();
            LockGuard lock(cli, argv[3], 60, /*heartbeat=*/false);
            cli.put_data(argv[3], argv[4], argv[5], lock.lock_id());
            std::cout << "written\n";
            return 0;
        }

        if (command == "mode") {
            if (argc < 5) return usage();
            LockGuard lock(cli, argv[3], 60, /*heartbeat=*/false);
            cli.set_mode(argv[3], argv[4], lock.lock_id());
            std::cout << "mode set\n";
            return 0;
        }

        if (command == "op") {
            if (argc < 5) return usage();
            std::string params = argc > 5 ? argv[5] : "{}";
            LockGuard lock(cli, argv[3], 60, /*heartbeat=*/false);
            std::cout << cli.execute_operation(argv[3], argv[4], params, lock.lock_id()).dump() << "\n";
            return 0;
        }

        if (command == "watch") {
            if (argc < 5) return usage();
            int interval_ms = argc > 5 ? std::atoi(argv[5]) : 1000;
            std::signal(SIGINT, on_sigint);
            std::signal(SIGTERM, on_sigint); // e.g. `timeout N sovd-cli ... watch ...` sends this, not SIGINT
            // Explicit flushes below matter here specifically: stdout is
            // fully (not line-) buffered whenever it isn't a tty -- a piped
            // or redirected `watch`, the exact case "live" output is for,
            // would otherwise show nothing until the process exits.
            std::cout << "watching " << argv[3] << "/" << argv[4] << " every " << interval_ms
                      << "ms via SSE (Ctrl-C to stop)\n";
            std::cout.flush();
            // Phase 6: pushed by the server's shared poller, not polled by
            // this loop -- "backed by adapter-level periodic read, not
            // per-request polling" (CLAUDE.md) applies to the client side
            // too: this is one long-lived subscription, not a GET per tick.
            std::string last;
            cli.subscribe_data(argv[3], argv[4], interval_ms,
                                [&](const nlohmann::json &event) {
                                    DataValue v = parse_data_value(event);
                                    std::string rendered = v.error ? *v.error : v.value.dump();
                                    if (rendered != last) {
                                        print_data_value(v);
                                        std::cout.flush();
                                        last = rendered;
                                    }
                                },
                                &g_stop);
            return 0;
        }

        return usage();
    } catch (const SovdError &ex) {
        std::cerr << "error: " << ex.what() << " (HTTP " << ex.status << ")\n";
        return 1;
    }
}
