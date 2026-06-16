#include "wl2/wl2.h"

#include <cassert>
#include <iostream>
#include <string>

static void run_buffer_tests() {
    auto a = wl2::Buffer::fromString("hello");
    auto b = a.slice(1, 3);
    assert(b.text() == "ell");

    auto view = b.mutableView();
    view[0] = std::byte{'a'};
    assert(b.text() == "all");
    assert(a.text() == "hello");
}

int main(int argc, char** argv);

int wl2_buffer_tests_entry() {
    run_buffer_tests();
    std::cout << "buffer ok\n";
    return 0;
}
