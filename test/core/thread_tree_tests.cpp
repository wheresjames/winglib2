#include "wl2/wl2.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>

int wl2_buffer_tests_entry();
int wl2_resource_tests_entry();
int wl2_runtime_tests_entry();
int wl2_async_host_tests_entry();
int wl2_async_integration_tests_entry();
int wl2_module_requirements_tests_entry();
int wl2_dynamic_module_tests_entry();
int wl2_module_store_tests_entry();
int wl2_module_deps_tests_entry();
int wl2_module_resolver_tests_entry();
int wl2_app_store_tests_entry();
int run_membus_tests();
int run_membus_test_case(std::string_view name);
int run_script_thread_tests();
int run_script_thread_test_case(std::string_view name);

static int run_thread_tree_tests() {
    wl2::ThreadTree tree;
    tree.create("/main/worker");
    assert(tree.send(wl2::Message{.id = 1, .source = "/main", .destination = "/main/worker", .type = "ping", .payload = "hello"}));
    auto node = tree.find("/main/worker");
    assert(node);
    auto message = node->mailbox().wait(std::chrono::milliseconds(10));
    assert(message);
    assert(message->payload == "hello");
    std::cout << "thread_tree ok\n";
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        return 2;
    }
    const std::string suite = argv[1];
    if (suite == "buffer") {
        return wl2_buffer_tests_entry();
    }
    if (suite == "resources") {
        return wl2_resource_tests_entry();
    }
    if (suite == "thread_tree") {
        return run_thread_tree_tests();
    }
    if (suite == "runtime") {
        return wl2_runtime_tests_entry();
    }
    if (suite == "async_host") {
        return wl2_async_host_tests_entry();
    }
    if (suite == "async_integration") {
        return wl2_async_integration_tests_entry();
    }
    if (suite == "module_requirements") {
        return wl2_module_requirements_tests_entry();
    }
    if (suite == "dynamic_module") {
        return wl2_dynamic_module_tests_entry();
    }
    if (suite == "module_store") {
        return wl2_module_store_tests_entry();
    }
    if (suite == "module_deps") {
        return wl2_module_deps_tests_entry();
    }
    if (suite == "module_resolver") {
        return wl2_module_resolver_tests_entry();
    }
    if (suite == "app_store") {
        return wl2_app_store_tests_entry();
    }
    if (suite == "script_thread") {
        if (argc > 2) {
            return run_script_thread_test_case(argv[2]);
        }
        return run_script_thread_tests();
    }
    if (suite == "membus") {
        if (argc > 2) {
            return run_membus_test_case(argv[2]);
        }
        return run_membus_tests();
    }
    return 2;
}
