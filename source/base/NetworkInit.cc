#include "NetworkInit.hpp"
#include "Logging.hpp"
#include <csignal>

void EnsureNetworkInitialized() {
    static const struct NetworkInit {
        NetworkInit() {
            DBG_LOG("SIGPIPE INIT");
            std::signal(SIGPIPE, SIG_IGN);
        }
    } instance;
    (void)instance;
}
