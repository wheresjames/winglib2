// A self-contained single executable that statically links wl2_asio_static and
// runs an embedded loopback echo script. It demonstrates two things at once:
// static single-executable linking of a native module, and the wl2:asio security
// model — the runner grants only loopback network/listen access, mirroring what
// `wl2 run --allow-network --network-allow 127.0.0.1 ...` would grant on the CLI.
#include "wl2/wl2.h"
#include "wl2/crash_report.h"
#include "wl2_asio/wl2_asio.h"

#include <iostream>

void wl2_register_embedded_resources(wl2::ResourceStore& store);

int main(int argc, char** argv) {
    wl2::crash::installFromArgs(argc, argv);

    wl2::RuntimeOptions options;
    options.allowFilesystem = false;
    // Least-privilege: only loopback connects and listeners are permitted.
    options.allowNetwork = true;
    options.networkAllowList = {"127.0.0.1"};
    options.allowListening = true;
    options.listenAllowList = {"127.0.0.1"};
    options.staticModules.push_back(wl2_asio_register_module);

    wl2::Runtime runtime(std::move(options));
    wl2_register_embedded_resources(runtime.resources());

    auto result = runtime.runModule("wl2:/tcp-echo/main.js");
    if (!result) {
        std::cerr << result.error().code() << ": " << result.error().message() << '\n';
        return 1;
    }
    return result.value();
}
