// A self-contained single executable that statically links wl2_restinio_static
// and runs an embedded HTTP/WebSocket server script. The example grants only the
// loopback listen capability; the UI is a plain HTML page served by the script.
#include "wl2/wl2.h"
#include "wl2/crash_report.h"
#include "wl2_restinio/wl2_restinio.h"

#include <iostream>

void wl2_register_embedded_resources(wl2::ResourceStore& store);

int main(int argc, char** argv) {
    wl2::crash::installFromArgs(argc, argv);

    wl2::RuntimeOptions options;
    options.allowFilesystem = false; // the script and HTML are embedded
    options.allowListening = true;
    options.listenAllowList = {"127.0.0.1"};
    for (int i = 1; i < argc; ++i) {
        options.scriptArgs.emplace_back(argv[i]);
    }
    options.staticModules.push_back(wl2_restinio_register_module);

    wl2::Runtime runtime(std::move(options));
    wl2_register_embedded_resources(runtime.resources());

    auto result = runtime.runModule("wl2:/http-showcase/main.js");
    if (!result) {
        std::cerr << result.error().code() << ": " << result.error().message() << '\n';
        return 1;
    }
    return result.value();
}
