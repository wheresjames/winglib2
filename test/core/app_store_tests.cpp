#include "wl2/app_store.h"

#include <iostream>

namespace {

int fail(const char* message) {
    std::cerr << "app store test failed: " << message << '\n';
    return 1;
}

int run() {
    auto github = wl2::normalizeAppSource("https://github.com/example/widgets/tree/v1.2.3/apps/panel");
    if (!github || github->repo != "https://github.com/example/widgets.git"
        || github->ref != "v1.2.3" || github->path != "apps/panel") {
        return fail("GitHub tree URL was not normalized");
    }

    auto pinned = wl2::normalizeAppSource("/tmp/widgets.git#release:apps/panel");
    if (!pinned || pinned->repo != "/tmp/widgets.git"
        || pinned->ref != "release" || pinned->path != "apps/panel") {
        return fail("repo#ref:path source was not parsed");
    }

    auto subdir = wl2::normalizeAppSource("/tmp/widgets.git:apps/panel");
    if (!subdir || subdir->repo != "/tmp/widgets.git"
        || !subdir->ref.empty() || subdir->path != "apps/panel") {
        return fail("repo:path source was not parsed");
    }

    if (wl2::appSlug("wl2:demo app") != "wl2_demo_app") {
        return fail("app slug did not sanitize name");
    }

    // A subdirectory that escapes the checkout must be rejected.
    if (wl2::normalizeAppSource("/tmp/widgets.git:../../etc")) {
        return fail("traversal subdirectory was accepted");
    }
    if (wl2::normalizeAppSource("/tmp/widgets.git:/etc/passwd")) {
        return fail("absolute subdirectory was accepted");
    }
    // A repo or ref that looks like a git option must be rejected.
    if (wl2::normalizeAppSource("--upload-pack=touch#main")) {
        return fail("option-like repo was accepted");
    }
    if (wl2::normalizeAppSource("/tmp/widgets.git#--evil")) {
        return fail("option-like ref was accepted");
    }

    std::cout << "app_store ok\n";
    return 0;
}

} // namespace

int wl2_app_store_tests_entry() {
    return run();
}
