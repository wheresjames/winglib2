#include "wl2/wl2.h"

#include <iostream>

void wl2_register_embedded_resources(wl2::ResourceStore& store);

int main() {
    wl2::ResourceStore store;
    wl2_register_embedded_resources(store);

    // Stored resources can expose a pointer directly to their embedded bytes.
    auto config = store.open("wl2:/resources/config.json");
    if (!config) {
        std::cerr << config.error().message() << '\n';
        return 1;
    }
    std::cout << "config bytes: " << config.value().size() << '\n';
    std::cout << "config raw pointer: " << config.value().data() << '\n';

    // Compressed resources expose the same handle API. The first open inflates
    // into an immutable cache; repeated opens can share that cached buffer.
    auto repeatedA = store.open("wl2:/resources/repeated.txt");
    auto repeatedB = store.open("wl2:/resources/repeated.txt");
    if (!repeatedA || !repeatedB) {
        std::cerr << "unable to open compressed resource\n";
        return 1;
    }
    std::cout << "compressed text: " << repeatedA.value().text() << '\n';
    std::cout << "cache shared: " << (repeatedA.value().data() == repeatedB.value().data()) << '\n';

    // Entire directories can be embedded recursively. Paths stay relative to
    // the configured resource root.
    auto nested = store.open("wl2:/resources/assets/nested/info.txt");
    if (!nested) {
        std::cerr << nested.error().message() << '\n';
        return 1;
    }
    std::cout << "nested text: " << nested.value().text() << '\n';

    if (store.exists("wl2:/resources/assets/skip.tmp")) {
        std::cerr << "excluded resource was embedded\n";
        return 1;
    }

    // Empty files embed as a valid, zero-length resource.
    auto empty = store.open("wl2:/resources/empty.txt");
    if (!empty) {
        std::cerr << empty.error().message() << '\n';
        return 1;
    }
    if (empty.value().size() != 0 || !empty.value().text().empty()) {
        std::cerr << "empty resource should have zero length\n";
        return 1;
    }
    std::cout << "empty bytes: " << empty.value().size() << '\n';

    // Resource paths can be explored like a read-only directory tree. list()
    // returns direct children only.
    for (const auto& entry : store.list("wl2:/resources")) {
        std::cout << (entry.directory ? "dir  " : "file ") << entry.path << '\n';
    }

    std::cout << "assets direct children:\n";
    for (const auto& entry : store.list("wl2:/resources/assets")) {
        std::cout << "  " << (entry.directory ? "dir  " : "file ") << entry.path << '\n';
    }

    // walk() is the concise way to enumerate all files below a directory.
    std::cout << "assets recursive files:\n";
    for (const auto& entry : store.walk("wl2:/resources/assets")) {
        std::cout << "  file " << entry.path << '\n';
    }

    return 0;
}
