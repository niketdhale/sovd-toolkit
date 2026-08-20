// Phase 8: mints a test bearer token for sovd_server's SOVD_OAUTH2_SECRET
// validation. There's no real IdP in this project to issue tokens (see
// oauth2.hpp's scoping note) -- this exists purely so the verification
// mechanism this project actually builds (and is responsible for) can be
// exercised live, the same way every other Phase 8 feature was.
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "sovd/server/oauth2.hpp"

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: sovd_mint_token <secret> <scope1,scope2,...> [ttl_seconds=3600]\n"
                     "example: sovd_mint_token my-secret read:data,read:faults 3600\n";
        return 1;
    }
    std::string secret = argv[1];
    std::vector<std::string> scopes;
    std::istringstream iss(argv[2]);
    for (std::string scope; std::getline(iss, scope, ','); ) {
        if (!scope.empty()) scopes.push_back(scope);
    }
    long ttl = argc > 3 ? std::atol(argv[3]) : 3600;

    std::cout << sovd::server::oauth2::mint_token(secret, scopes, ttl) << std::endl;
    return 0;
}
