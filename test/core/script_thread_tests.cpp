#include "wl2/wl2.h"

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>

using namespace std::chrono_literals;

namespace {

int fail(std::string_view message) {
    std::cerr << "script_thread test failure: " << message << '\n';
    return 1;
}

wl2::Message work_message(std::string destination, std::string payload) {
    wl2::Message message;
    message.source = "/main";
    message.destination = std::move(destination);
    message.type = "work";
    message.payload = std::move(payload);
    return message;
}

// Spawn a child script and confirm it runs to completion with a success result.
int test_spawn_success() {
    wl2::Runtime runtime;
    wl2::ScriptThreadOptions options;
    options.path = "/main/worker";
    options.source = "const value = 1 + 1; void value;";

    auto spawned = runtime.threadTree().spawn(runtime, options);
    if (!spawned) {
        return fail("spawn returned an error");
    }
    auto thread = spawned.value();
    if (!thread->wait(2s)) {
        return fail("spawned script did not finish");
    }
    if (thread->failed()) {
        return fail("spawned script reported failure");
    }
    if (!thread->result()) {
        return fail("spawned script result is not ok");
    }
    return 0;
}

// A throwing child script surfaces as a failed result on the parent side.
int test_child_failure() {
    wl2::Runtime runtime;
    wl2::ScriptThreadOptions options;
    options.path = "/main/worker";
    options.source = "throw new Error('boom');";

    auto spawned = runtime.threadTree().spawn(runtime, options);
    if (!spawned) {
        return fail("spawn returned an error");
    }
    auto thread = spawned.value();
    if (!thread->wait(2s)) {
        return fail("failing script did not finish");
    }
    if (!thread->failed()) {
        return fail("failing script was not reported as failed");
    }
    if (thread->result()) {
        return fail("failing script result should not be ok");
    }
    return 0;
}

// Spawn rejects invalid and duplicate paths before creating ambiguous routing.
int test_spawn_path_validation() {
    wl2::Runtime runtime;

    wl2::ScriptThreadOptions relative;
    relative.path = "worker";
    relative.source = "void 0;";
    if (runtime.threadTree().spawn(runtime, relative)) {
        return fail("relative thread path should be rejected");
    }

    wl2::ScriptThreadOptions dots;
    dots.path = "/main/../worker";
    dots.source = "void 0;";
    if (runtime.threadTree().spawn(runtime, dots)) {
        return fail("dot-segment thread path should be rejected");
    }

    wl2::ScriptThreadOptions root;
    root.path = "/main";
    root.source = "void 0;";
    if (runtime.threadTree().spawn(runtime, root)) {
        return fail("spawning /main should be rejected");
    }

    wl2::ScriptThreadOptions first;
    first.path = "/main/worker";
    first.source = "void 0;";
    auto spawned = runtime.threadTree().spawn(runtime, first);
    if (!spawned) {
        return fail("valid spawn returned an error");
    }

    wl2::ScriptThreadOptions duplicate;
    duplicate.path = "/main/worker";
    duplicate.source = "void 0;";
    if (runtime.threadTree().spawn(runtime, duplicate)) {
        return fail("duplicate thread path should be rejected");
    }
    spawned.value()->join();
    return 0;
}

// A one-way message arrives as a ThreadRequest that does not expect a reply.
int test_one_way_message() {
    wl2::ThreadTree tree;
    tree.create("/main/worker");
    if (!tree.send(work_message("/main/worker", "frame-1"))) {
        return fail("send to existing node failed");
    }
    auto request = tree.take("/main/worker", 100ms);
    if (!request) {
        return fail("take did not return the queued message");
    }
    if (request->expectsReply()) {
        return fail("one-way message should not expect a reply");
    }
    if (request->payload() != "frame-1") {
        return fail("one-way payload mismatch");
    }
    if (request->reply("ignored")) {
        return fail("replying to a one-way message should report no caller");
    }
    return 0;
}

// Request/reply round trip resolves the caller's pending reply with the payload.
int test_request_reply_success() {
    wl2::ThreadTree tree;
    tree.create("/main/worker");

    auto pending = tree.request(work_message("/main/worker", "input"), 1000ms);
    auto request = tree.take("/main/worker", 100ms);
    if (!request) {
        return fail("responder did not receive the request");
    }
    if (!request->expectsReply()) {
        return fail("request should expect a reply");
    }
    if (!request->reply(request->payload() + "-done")) {
        return fail("reply was not routed to the caller");
    }

    auto reply = pending.wait();
    if (!reply.ok()) {
        return fail("caller did not receive an ok reply");
    }
    if (reply.payload != "input-done") {
        return fail("reply payload mismatch");
    }
    return 0;
}

// A responder reject surfaces to the caller as a rejected reply.
int test_request_reject() {
    wl2::ThreadTree tree;
    tree.create("/main/worker");

    auto pending = tree.request(work_message("/main/worker", "input"), 1000ms);
    auto request = tree.take("/main/worker", 100ms);
    if (!request) {
        return fail("responder did not receive the request");
    }
    request->reject("unsupported");

    auto reply = pending.wait();
    if (reply.status != wl2::ReplyStatus::Rejected) {
        return fail("caller did not receive a rejection");
    }
    if (reply.error != "unsupported") {
        return fail("rejection error mismatch");
    }
    return 0;
}

// Dropping an unresolved request rejects it so the caller never hangs.
int test_request_dropped() {
    wl2::ThreadTree tree;
    tree.create("/main/worker");

    auto pending = tree.request(work_message("/main/worker", "input"), 1000ms);
    {
        auto request = tree.take("/main/worker", 100ms);
        if (!request) {
            return fail("responder did not receive the request");
        }
        // request goes out of scope without reply() or reject().
    }

    auto reply = pending.wait();
    if (reply.status != wl2::ReplyStatus::Rejected) {
        return fail("dropped request did not reject the caller");
    }
    if (reply.error != "request_dropped") {
        return fail("dropped request error mismatch");
    }
    return 0;
}

// A request with no live responder times out at the deadline.
int test_request_timeout() {
    wl2::ThreadTree tree;
    tree.create("/main/worker");

    auto pending = tree.request(work_message("/main/worker", "input"), 40ms);
    auto reply = pending.wait();
    if (reply.status != wl2::ReplyStatus::Timeout) {
        return fail("request did not time out");
    }
    if (!pending.done()) {
        return fail("pending should be done after timeout");
    }
    return 0;
}

// A reply after the deadline does not complete the caller; it is dropped to
// diagnostics and the reply token is expired.
int test_late_reply() {
    wl2::ThreadTree tree;
    tree.create("/main/worker");

    auto pending = tree.request(work_message("/main/worker", "input"), 30ms);
    auto request = tree.take("/main/worker", 100ms);
    if (!request) {
        return fail("responder did not receive the request");
    }

    auto reply = pending.wait();
    if (reply.status != wl2::ReplyStatus::Timeout) {
        return fail("caller should have timed out before the late reply");
    }
    if (tree.droppedReplies() != 0) {
        return fail("no reply should have been dropped yet");
    }

    // The reply token is now expired: routing it counts as a dropped reply and
    // does not change the caller's terminal Timeout.
    request->reply("late");
    if (tree.droppedReplies() != 1) {
        return fail("late reply was not routed to diagnostics");
    }
    if (!pending.done() || pending.poll()->status != wl2::ReplyStatus::Timeout) {
        return fail("late reply must not overwrite the timeout result");
    }
    return 0;
}

// Requests to an unknown destination resolve immediately as unreachable.
int test_request_unreachable() {
    wl2::ThreadTree tree;
    auto pending = tree.request(work_message("/main/ghost", "input"), 1000ms);
    auto reply = pending.poll();
    if (!reply) {
        return fail("unreachable request should resolve immediately");
    }
    if (reply->status != wl2::ReplyStatus::Unreachable) {
        return fail("missing destination should be unreachable");
    }
    return 0;
}

// Cancelling a pending reply resolves it and drops any later reply.
int test_cancel() {
    wl2::ThreadTree tree;
    tree.create("/main/worker");

    auto pending = tree.request(work_message("/main/worker", "input"), 1000ms);
    auto request = tree.take("/main/worker", 100ms);
    if (!request) {
        return fail("responder did not receive the request");
    }
    pending.cancel();

    auto reply = pending.wait();
    if (reply.status != wl2::ReplyStatus::Cancelled) {
        return fail("cancel did not resolve the pending reply");
    }
    request->reply("too late");
    if (tree.droppedReplies() != 1) {
        return fail("reply after cancel should be dropped");
    }
    return 0;
}

// Pending requests to a path are rejected when that thread exits.
int test_reject_on_exit() {
    wl2::ThreadTree tree;
    tree.create("/main/worker");

    auto pending = tree.request(work_message("/main/worker", "input"), 5000ms);
    tree.rejectPendingFor("/main/worker", "thread_exited");

    auto reply = pending.wait();
    if (reply.status != wl2::ReplyStatus::Unreachable) {
        return fail("exit did not reject the pending request");
    }
    if (reply.error != "thread_exited") {
        return fail("exit rejection error mismatch");
    }
    return 0;
}

// Shutdown completes even when nodes still hold queued messages.
int test_shutdown_with_queued() {
    wl2::ThreadTree tree;
    tree.create("/main/worker");
    tree.send(work_message("/main/worker", "a"));
    tree.send(work_message("/main/worker", "b"));

    tree.shutdown();

    // After shutdown the mailbox is closed: a wait returns promptly with nothing
    // queued for new readers beyond what was already buffered.
    auto node = tree.find("/main/worker");
    if (!node) {
        return fail("node should still be addressable after shutdown");
    }
    return 0;
}

// A spawned thread's node can be cleaned up after it finishes.
int test_path_cleanup() {
    wl2::Runtime runtime;
    wl2::ScriptThreadOptions options;
    options.path = "/main/worker";
    options.source = "void 0;";

    auto spawned = runtime.threadTree().spawn(runtime, options);
    if (!spawned) {
        return fail("spawn returned an error");
    }
    spawned.value()->join();

    if (!runtime.threadTree().find("/main/worker")) {
        return fail("spawned node should exist before cleanup");
    }
    if (!runtime.threadTree().remove("/main/worker")) {
        return fail("removing the spawned node failed");
    }
    if (runtime.threadTree().find("/main/worker")) {
        return fail("node was not cleaned up");
    }
    return 0;
}

// A runaway pure-JS script is stopped when its thread is interrupted.
int test_interrupt() {
    wl2::Runtime runtime;
    wl2::ScriptThreadOptions options;
    options.path = "/main/loop";
    options.source = "while (true) {}";

    auto spawned = runtime.threadTree().spawn(runtime, options);
    if (!spawned) {
        return fail("spawn returned an error");
    }
    auto thread = spawned.value();
    thread->interrupt();
    if (!thread->wait(5s)) {
        return fail("interrupt did not stop the runaway script");
    }
    if (!thread->failed()) {
        return fail("interrupted script should report failure");
    }
    return 0;
}

// shutdown(grace) waits, then force-stops a runaway child within a bound.
int test_force_close() {
    wl2::Runtime runtime;
    wl2::ScriptThreadOptions options;
    options.path = "/main/loop";
    options.source = "while (true) {}";

    auto spawned = runtime.threadTree().spawn(runtime, options);
    if (!spawned) {
        return fail("spawn returned an error");
    }
    auto thread = spawned.value();

    const auto start = std::chrono::steady_clock::now();
    runtime.threadTree().shutdown(100ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    if (!thread->finished()) {
        return fail("runaway thread was not stopped by shutdown");
    }
    if (!thread->failed()) {
        return fail("force-closed thread should report failure");
    }
    if (elapsed > 5s) {
        return fail("forced shutdown took too long");
    }
    return 0;
}

// A child may initiate shutdown without attempting to join or destroy itself.
int test_child_initiated_shutdown() {
    wl2::Runtime runtime;
    wl2::ScriptThreadOptions options;
    options.path = "/main/worker";
    options.source = "wl2.thread.shutdown({ timeoutMs: 10 });";

    auto spawned = runtime.threadTree().spawn(runtime, options);
    if (!spawned) {
        return fail("spawn returned an error");
    }
    auto thread = spawned.value();
    if (!thread->wait(2s)) {
        return fail("child-initiated shutdown did not return");
    }
    if (thread->failed()) {
        return fail("child-initiated shutdown reported failure");
    }
    thread->join();
    return 0;
}

// Well-behaved threads finish during the graceful window without being killed.
int test_graceful_shutdown() {
    wl2::Runtime runtime;
    std::vector<std::shared_ptr<wl2::ScriptThread>> threads;
    for (int i = 0; i < 4; ++i) {
        wl2::ScriptThreadOptions options;
        options.path = "/main/worker" + std::to_string(i);
        options.source = "void 0;";
        auto spawned = runtime.threadTree().spawn(runtime, options);
        if (!spawned) {
            return fail("spawn returned an error");
        }
        threads.push_back(spawned.value());
    }

    runtime.threadTree().shutdown();

    for (auto& thread : threads) {
        if (!thread->finished()) {
            return fail("thread did not finish during graceful shutdown");
        }
        if (thread->failed()) {
            return fail("well-behaved thread should not be reported as failed");
        }
    }
    return 0;
}

// A child reports a clean exit to its parent over the tree transport.
int test_child_exit_report() {
    wl2::Runtime runtime;
    wl2::ScriptThreadOptions options;
    options.path = "/main/worker";
    options.source = "void 0;";

    auto spawned = runtime.threadTree().spawn(runtime, options);
    if (!spawned) {
        return fail("spawn returned an error");
    }
    spawned.value()->join();

    auto message = runtime.threadTree().take("/main", 2s);
    if (!message) {
        return fail("parent did not receive an exit message");
    }
    if (message->type() != "thread-exit") {
        return fail("exit message type mismatch");
    }
    if (message->source() != "/main/worker") {
        return fail("exit message source mismatch");
    }
    return 0;
}

// A failing child reports an error, with its error code, to its parent.
int test_child_error_report() {
    wl2::Runtime runtime;
    wl2::ScriptThreadOptions options;
    options.path = "/main/worker";
    options.source = "throw new Error('boom');";

    auto spawned = runtime.threadTree().spawn(runtime, options);
    if (!spawned) {
        return fail("spawn returned an error");
    }
    spawned.value()->join();

    auto message = runtime.threadTree().take("/main", 2s);
    if (!message) {
        return fail("parent did not receive an error message");
    }
    if (message->type() != "thread-error") {
        return fail("error message type mismatch");
    }
    if (message->payload().empty()) {
        return fail("error message should carry an error code");
    }
    return 0;
}

// The opt-in subtree policy interrupts a failed thread's descendants.
int test_failure_subtree() {
    wl2::Runtime runtime;

    // A runaway descendant, spawned first so it is present when the parent fails.
    wl2::ScriptThreadOptions child;
    child.path = "/main/parent/child";
    child.source = "while (true) {}";
    auto childSpawn = runtime.threadTree().spawn(runtime, child);
    if (!childSpawn) {
        return fail("child spawn returned an error");
    }
    auto childThread = childSpawn.value();

    // The parent fails and is configured to tear down its subtree.
    wl2::ScriptThreadOptions parent;
    parent.path = "/main/parent";
    parent.source = "throw new Error('fail');";
    parent.onFailure = wl2::ThreadFailurePolicy::ShutdownSubtree;
    auto parentSpawn = runtime.threadTree().spawn(runtime, parent);
    if (!parentSpawn) {
        return fail("parent spawn returned an error");
    }

    if (!childThread->wait(5s)) {
        return fail("subtree policy did not interrupt the descendant");
    }
    if (!childThread->failed()) {
        return fail("interrupted descendant should report failure");
    }
    return 0;
}

// JavaScript-facing wl2.thread covers spawn, post, request/reply, rejection,
// timeout, parent/child discovery, shutdown, and byte payloads.
int test_javascript_thread_api() {
    wl2::Runtime runtime;
    auto engine = wl2::createConfiguredJsEngine();

    const char* source = R"JS(
function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

assert(wl2.thread.path === "/main", "main thread path mismatch");
assert(wl2.thread.parent === null, "main parent should be null");

const childSource = `
if (wl2.thread.path !== "/main/worker1") {
  throw new Error("child path mismatch: " + wl2.thread.path);
}
if (wl2.thread.parent !== "/main") {
  throw new Error("child parent mismatch");
}
wl2.thread.post("/main", { type: "child-ready", payload: wl2.buffer.fromText("ready") });

const parentReply = wl2.thread.request("/main", { type: "need-parent", payload: "question" }, { timeoutMs: 2000 });
if (!parentReply.ok || parentReply.payload !== "answer") {
  throw new Error("child did not receive parent reply");
}

const post = wl2.thread.recv({ timeoutMs: 2000 });
if (!post || post.type !== "hello" || post.payload !== "fire") {
  throw new Error("child did not receive parent post");
}

const req = wl2.thread.recv({ timeoutMs: 2000 });
if (!req || req.type !== "compute") {
  throw new Error("child did not receive compute request");
}
req.reply(wl2.buffer.fromText(req.payload + "-done"));

const pendingReq = wl2.thread.recv({ timeoutMs: 2000 });
if (!pendingReq || pendingReq.type !== "compute") {
  throw new Error("child did not receive pending compute request");
}
pendingReq.reply(pendingReq.payload + "-done");

const cancelled = wl2.thread.recv({ timeoutMs: 2000 });
if (!cancelled || cancelled.type !== "cancelled") {
  throw new Error("child did not receive cancelled request");
}
// Dropping this request exercises the caller-side cancellation path.

const afterCancel = wl2.thread.recv({ timeoutMs: 2000 });
if (!afterCancel || afterCancel.type !== "compute") {
  throw new Error("child did not receive post-cancel compute request");
}
afterCancel.reply(afterCancel.payload + "-done");

const rejected = wl2.thread.recv({ timeoutMs: 2000 });
if (!rejected || rejected.type !== "reject-me") {
  throw new Error("child did not receive reject request");
}
rejected.reject("nope");

const bytes = wl2.thread.recv({ timeoutMs: 2000 });
if (!bytes || bytes.payload !== "ABC") {
  throw new Error("child did not receive byte payload");
}
`;

const worker = wl2.thread.spawn("inline:worker", { name: "worker1", source: childSource });
assert(worker.path === "/main/worker1", "worker path mismatch");
assert(wl2.thread.children().indexOf("/main/worker1") >= 0, "child not listed");

const ready = wl2.thread.recv({ timeoutMs: 2000 });
assert(ready && ready.type === "child-ready" && ready.payload === "ready", "ready message mismatch");

const parentReq = wl2.thread.recv({ timeoutMs: 2000 });
assert(parentReq && parentReq.type === "need-parent", "parent did not receive child request");
assert(parentReq.reply("answer"), "parent reply failed");

assert(worker.post({ type: "hello", payload: "fire" }), "worker.post failed");

const result = worker.request({ type: "compute", payload: "input" }, { timeoutMs: 2000 });
assert(result.ok && result.payload === "input-done", "worker request reply mismatch");

const pending = worker.requestPending({ type: "compute", payload: "pending" }, { timeoutMs: 2000 });
assert(pending.id > 0 && pending.done === false, "pending metadata mismatch");
assert(pending.poll() === null, "pending request should not be done before receive");
const pendingReply = pending.wait({ timeoutMs: 2000 });
assert(pendingReply.ok && pendingReply.payload === "pending-done", "pending wait mismatch");
assert(pending.done === true, "pending should be done after wait");

const cancelPending = wl2.thread.requestPending(worker.path, { type: "cancelled", payload: "" }, { timeoutMs: 2000 });
assert(cancelPending.poll() === null, "cancel pending should start unresolved");
cancelPending.cancel();
const cancelReply = cancelPending.wait({ timeoutMs: 2000 });
assert(cancelReply.status === "cancelled", "pending cancel mismatch");
const cancelledReq = worker.request({ type: "compute", payload: "after-cancel" }, { timeoutMs: 2000 });
assert(cancelledReq.ok && cancelledReq.payload === "after-cancel-done", "worker should continue after cancel");

const rejected = worker.request({ type: "reject-me", payload: "" }, { timeoutMs: 2000 });
assert(!rejected.ok && rejected.status === "rejected" && rejected.error === "nope", "reject mismatch");

assert(worker.post({ type: "bytes", payload: wl2.buffer.fromText("ABC") }), "byte post failed");
assert(worker.wait({ timeoutMs: 2000 }), "worker did not finish");
worker.close();

const timeout = wl2.thread.request("/main", { type: "nobody", payload: "" }, { timeoutMs: 20 });
assert(timeout.status === "timeout", "self request should time out");
let awaitRejected = false;
try {
  await wl2.thread.requestPending("/main", { type: "await-timeout", payload: "" }, { timeoutMs: 20 });
} catch (error) {
  awaitRejected = error.status === "timeout";
}
assert(awaitRejected, "awaiting a timed-out pending reply should reject");
for (const pending of wl2.thread.requests()) {
  pending.reject("drained");
}

wl2.thread.shutdown({ timeoutMs: 20 });
)JS";

    auto result = engine->runModule(runtime, "thread-api-test.js", source);
    if (!result) {
        std::cerr << "JS thread API error: " << result.error().code()
                  << ": " << result.error().message() << '\n';
        return fail("javascript thread API failed");
    }
    return 0;
}

// Stress: many short-lived threads and many request/reply round trips.
int test_stress() {
    {
        wl2::Runtime runtime;
        std::vector<std::shared_ptr<wl2::ScriptThread>> threads;
        for (int i = 0; i < 32; ++i) {
            wl2::ScriptThreadOptions options;
            options.path = "/main/worker" + std::to_string(i);
            options.source = "const n = " + std::to_string(i) + "; void n;";
            auto spawned = runtime.threadTree().spawn(runtime, options);
            if (!spawned) {
                return fail("stress spawn failed");
            }
            threads.push_back(spawned.value());
        }
        for (auto& thread : threads) {
            if (!thread->wait(5s) || thread->failed()) {
                return fail("stress thread did not finish cleanly");
            }
        }
    }

    {
        wl2::ThreadTree tree;
        tree.create("/main/worker");
        for (int i = 0; i < 500; ++i) {
            auto pending = tree.request(work_message("/main/worker", std::to_string(i)), 1000ms);
            auto request = tree.take("/main/worker", 100ms);
            if (!request) {
                return fail("stress request was not received");
            }
            request->reply(request->payload());
            auto reply = pending.wait();
            if (!reply.ok() || reply.payload != std::to_string(i)) {
                return fail("stress request/reply mismatch");
            }
        }
        if (tree.droppedReplies() != 0) {
            return fail("stress run dropped replies unexpectedly");
        }
    }
    return 0;
}

struct Case {
    std::string_view name;
    int (*fn)();
};

const Case kCases[] = {
    {"spawn_success", test_spawn_success},
    {"child_failure", test_child_failure},
    {"spawn_path_validation", test_spawn_path_validation},
    {"one_way", test_one_way_message},
    {"request_reply", test_request_reply_success},
    {"request_reject", test_request_reject},
    {"request_dropped", test_request_dropped},
    {"request_timeout", test_request_timeout},
    {"late_reply", test_late_reply},
    {"request_unreachable", test_request_unreachable},
    {"cancel", test_cancel},
    {"reject_on_exit", test_reject_on_exit},
    {"shutdown_queued", test_shutdown_with_queued},
    {"path_cleanup", test_path_cleanup},
    {"interrupt", test_interrupt},
    {"force_close", test_force_close},
    {"child_initiated_shutdown", test_child_initiated_shutdown},
    {"graceful_shutdown", test_graceful_shutdown},
    {"child_exit_report", test_child_exit_report},
    {"child_error_report", test_child_error_report},
    {"failure_subtree", test_failure_subtree},
    {"javascript_api", test_javascript_thread_api},
};

} // namespace

int run_script_thread_test_case(std::string_view name) {
    if (name == "stress") {
        return test_stress();
    }
    for (const auto& test : kCases) {
        if (test.name == name) {
            return test.fn();
        }
    }
    std::cerr << "unknown script_thread case: " << name << '\n';
    return 2;
}

int run_script_thread_tests() {
    for (const auto& test : kCases) {
        if (int rc = test.fn(); rc != 0) {
            std::cerr << "failed case: " << test.name << '\n';
            return rc;
        }
    }
    std::cout << "script_thread ok\n";
    return 0;
}
