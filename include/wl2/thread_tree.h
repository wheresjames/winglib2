#pragma once

/**
 * @file thread_tree.h
 * @brief Script-thread tree and mailbox message primitives.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "wl2/errors.h"

namespace wl2 {

class Runtime;

/**
 * @brief Current script-thread path for the calling OS thread.
 * @return The active thread-tree path, or `/main` for the host thread.
 */
const std::string& currentThreadPath();

/**
 * @brief RAII guard that publishes a script-thread path to native bindings.
 */
class ScopedThreadPath {
public:
    /**
     * @brief Publish path as the current thread-tree path for this OS thread.
     * @param path Absolute thread-tree path to expose to native bindings.
     */
    explicit ScopedThreadPath(std::string path);

    /// Restore the previous thread-tree path for this OS thread.
    ~ScopedThreadPath();

    ScopedThreadPath(const ScopedThreadPath&) = delete;
    ScopedThreadPath& operator=(const ScopedThreadPath&) = delete;

private:
    std::string previous_;
};

/**
 * @brief Message passed between nodes in a ThreadTree.
 *
 * The current payload is a string while the runtime model is still small.
 * Future revisions should allow structured values and shared buffer handles.
 */
struct Message {
    /// Message identifier assigned by the sender.
    uint64_t id = 0;

    /// Request identifier this message replies to, or zero.
    uint64_t replyTo = 0;

    /// Source node path, for example `/main`.
    std::string source;

    /// Destination node path, for example `/main/worker`.
    std::string destination;

    /// Application-defined message type.
    std::string type;

    /// Message payload.
    std::string payload;

    /// True when the sender is waiting for a reply through a PendingReply.
    bool expectsReply = false;
};

/// Message and request/reply share the same wire type.
using ThreadMessage = Message;

/**
 * @brief Thread-safe FIFO mailbox for one thread-tree node.
 *
 * Mailbox is the synchronization primitive below the script-thread API.
 * Producers post messages, while the owner waits with a timeout. close()
 * wakes waiters and causes future waits to return no message when the queue is
 * empty.
 *
 * @code{.cpp}
 * wl2::Mailbox mailbox;
 * mailbox.post({.type = "ready", .payload = "ok"});
 *
 * auto message = mailbox.wait(std::chrono::milliseconds{100});
 * if (message) {
 *     std::cout << message->type << "\n";
 * }
 * @endcode
 */
class Mailbox {
public:
    /**
     * @brief Append a message and wake one waiter.
     * @param message Message to enqueue.
     */
    void post(Message message);

    /**
     * @brief Wait for a message until timeout or close.
     * @param timeout Maximum time to wait.
     * @return Next message, or std::nullopt when timed out or closed with no
     * queued messages.
     */
    std::optional<Message> wait(std::chrono::milliseconds timeout);

    /**
     * @brief Current queued message count.
     * @return Number of queued messages.
     */
    size_t size() const;

    /// Close the mailbox and wake waiters.
    void close();

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Message> messages_;
    bool closed_ = false;
};

/**
 * @brief One addressable node in the script thread tree.
 *
 * A node has a stable path and a mailbox. The current C++ type does not own an
 * operating-system thread yet; it is the addressable coordination primitive
 * used by higher-level script-thread support.
 */
class ThreadNode {
public:
    /**
     * @brief Create a node with an absolute tree path.
     * @param path Absolute path for this node.
     */
    explicit ThreadNode(std::string path);

    /**
     * @brief Absolute node path.
     * @return Node path string.
     */
    const std::string& path() const noexcept { return path_; }

    /**
     * @brief Node mailbox.
     * @return Mutable mailbox for this node.
     */
    Mailbox& mailbox() noexcept { return mailbox_; }

private:
    std::string path_;
    Mailbox mailbox_;
};

/**
 * @brief Terminal outcome of a request.
 */
enum class ReplyStatus {
    /// The responder called reply().
    Ok,
    /// The responder called reject() or dropped the request.
    Rejected,
    /// The request deadline passed before a reply arrived.
    Timeout,
    /// The caller cancelled the pending reply.
    Cancelled,
    /// The destination did not exist or its thread exited.
    Unreachable,
};

/**
 * @brief Result delivered back to the caller of ThreadTree::request().
 */
struct ThreadReply {
    /// Terminal status of the request.
    ReplyStatus status = ReplyStatus::Ok;
    /// Request identifier this reply resolves.
    uint64_t requestId = 0;
    /// Responder path, or the destination path for synthesized terminal states.
    std::string source;
    /// Application-defined reply type.
    std::string type;
    /// Reply payload, valid when status is Ok.
    std::string payload;
    /// Diagnostic detail for non-Ok terminal states.
    std::string error;

    /// @return True when the responder replied successfully.
    bool ok() const noexcept { return status == ReplyStatus::Ok; }
};

/// Internal shared state behind a PendingReply. Defined in the implementation.
struct PendingReplyState;

class ThreadTree;

/**
 * @brief Caller-side handle for an in-flight request.
 *
 * A PendingReply is returned by ThreadTree::request(). It resolves exactly once,
 * to a reply, rejection, timeout, cancellation, or unreachable-destination
 * outcome. The reply is marshaled back to the requesting thread rather than
 * delivered to a mailbox, so the caller simply waits on the handle.
 *
 * A PendingReply must not outlive the ThreadTree that produced it.
 *
 * @code{.cpp}
 * auto pending = tree.request({.destination = "/main/worker",
 *                              .type = "work",
 *                              .payload = "input"},
 *                             std::chrono::milliseconds{1000});
 * ThreadReply reply = pending.wait();
 * if (reply.ok()) {
 *     // use reply.payload
 * }
 * @endcode
 */
class PendingReply {
public:
    PendingReply() = default;

    /// @return True when this handle is bound to a request.
    bool valid() const noexcept { return state_ != nullptr; }

    /// @return Request identifier, or zero for an empty handle.
    uint64_t id() const noexcept;

    /// @return True once the request has reached a terminal state.
    bool done() const;

    /**
     * @brief Non-blocking check for a terminal reply.
     * @return The terminal reply when resolved, otherwise std::nullopt.
     */
    std::optional<ThreadReply> poll() const;

    /**
     * @brief Wait up to timeout for the request to resolve.
     * @param timeout Maximum time to wait.
     * @return The terminal reply when resolved (including a deadline timeout),
     * or std::nullopt when the request is still pending after timeout.
     */
    std::optional<ThreadReply> wait(std::chrono::milliseconds timeout) const;

    /**
     * @brief Wait until the request resolves or its deadline passes.
     * @return The terminal reply, synthesizing a Timeout at the deadline.
     */
    ThreadReply wait() const;

    /// Cancel the request. A later reply is dropped to diagnostics.
    void cancel();

private:
    friend class ThreadTree;
    explicit PendingReply(std::shared_ptr<PendingReplyState> state);
    std::shared_ptr<PendingReplyState> state_;
};

/**
 * @brief Receiver-side view of an inbound message.
 *
 * ThreadTree::take() wraps the next inbound message as a ThreadRequest. For a
 * request (expectsReply() is true) the receiver must resolve it exactly once
 * with reply() or reject(); destroying an unresolved, non-deferred request
 * rejects it so the caller is never left waiting. One-way messages have
 * expectsReply() false and ignore reply()/reject().
 *
 * ThreadRequest is move-only.
 *
 * @code{.cpp}
 * if (auto req = tree.take("/main/worker", std::chrono::milliseconds{100})) {
 *     if (req->type() == "work") {
 *         req->reply(req->payload() + "-done");
 *     } else {
 *         req->reject("unknown request");
 *     }
 * }
 * @endcode
 */
class ThreadRequest {
public:
    ThreadRequest() = default;

    /**
     * @brief Move a request handle and its reply obligation.
     * @param other Request to move from.
     */
    ThreadRequest(ThreadRequest&& other) noexcept;

    /**
     * @brief Replace this request with another moved request.
     * @param other Request to move from.
     * @return This request.
     */
    ThreadRequest& operator=(ThreadRequest&& other) noexcept;
    ThreadRequest(const ThreadRequest&) = delete;
    ThreadRequest& operator=(const ThreadRequest&) = delete;
    ~ThreadRequest();

    /// @return Request identifier.
    uint64_t id() const noexcept { return message_.id; }
    /// @return Source path of the message.
    const std::string& source() const noexcept { return message_.source; }
    /// @return Destination path of the message.
    const std::string& destination() const noexcept { return message_.destination; }
    /// @return Application-defined message type.
    const std::string& type() const noexcept { return message_.type; }
    /// @return Message payload.
    const std::string& payload() const noexcept { return message_.payload; }
    /// @return Underlying message.
    const Message& message() const noexcept { return message_; }
    /// @return True when the sender is waiting for a reply.
    bool expectsReply() const noexcept { return message_.expectsReply; }
    /// @return True once reply() or reject() has been called.
    bool resolved() const noexcept { return resolved_; }

    /**
     * @brief Reply to the request.
     * @param payload Reply payload.
     * @param type Reply type.
     * @return True when the reply was routed to a waiting caller.
     */
    bool reply(std::string payload, std::string type = "reply");

    /**
     * @brief Reject the request.
     * @param error Diagnostic message describing the failure.
     * @return True when the rejection was routed to a waiting caller.
     */
    bool reject(std::string error);

    /// Take ownership of the reply obligation for later asynchronous handling.
    void defer() noexcept { deferred_ = true; }

private:
    friend class ThreadTree;
    ThreadRequest(ThreadTree* tree, Message message);
    ThreadTree* tree_ = nullptr;
    Message message_;
    bool resolved_ = false;
    bool deferred_ = false;
};

/**
 * @brief What happens to a child's subtree when the child fails.
 */
enum class ThreadFailurePolicy {
    /// A child failure is reported but affects nothing else (default).
    Isolate,
    /// A child failure also interrupts the child's descendant threads.
    ShutdownSubtree,
};

/**
 * @brief Options for launching a script thread.
 */
struct ScriptThreadOptions {
    /// Absolute tree path for the new thread, for example `/main/worker`.
    std::string path;
    /// Resource or filesystem specifier for the script, for example
    /// `wl2:/workers/worker.js`. Ignored when `source` is set.
    std::string script;
    /// Inline script source. When non-empty it overrides `script`.
    std::string source;
    /// Post a `thread-exit` message to the parent node when the thread ends.
    bool reportExitToParent = true;
    /// What a failure of this thread does to its descendants.
    ThreadFailurePolicy onFailure = ThreadFailurePolicy::Isolate;
};

/**
 * @brief A running script on its own operating-system thread.
 *
 * Each ScriptThread owns an independent JavaScript engine instance, so all
 * JS-visible work for the thread runs only on that thread. Cross-thread
 * communication goes through the thread tree, never by sharing engine state.
 *
 * The script runs to completion; result() reports its exit code or the error
 * that ended it. join() and the ThreadTree shutdown path wait for completion
 * deterministically. A runaway pure-JS script can be stopped early with
 * interrupt(), which aborts the script at the next engine checkpoint; a script
 * blocked inside a native call is not interruptible.
 *
 * ScriptThread instances are created through ThreadTree::spawn().
 */
class ScriptThread {
public:
    ~ScriptThread();

    ScriptThread(const ScriptThread&) = delete;
    ScriptThread& operator=(const ScriptThread&) = delete;

    /// @return Absolute tree path for this thread.
    const std::string& path() const noexcept { return path_; }

    /// @return True once the script has finished running.
    bool finished() const;

    /// @return True when the script ended with an error.
    bool failed() const;

    /**
     * @brief Wait up to timeout for the script to finish.
     * @param timeout Maximum time to wait.
     * @return True when the script has finished.
     */
    bool wait(std::chrono::milliseconds timeout) const;

    /// Block until the script finishes and join the operating-system thread.
    void join();

    /**
     * @brief Check whether this ScriptThread is the calling OS thread.
     * @return True when called from the script thread itself.
     */
    bool isCurrentThread() const noexcept;

    /**
     * @brief Request that a running script abort at the next engine checkpoint.
     *
     * Cooperative and idempotent. Has no effect once the script has finished or
     * while it is blocked inside a native call.
     */
    void interrupt();

    /**
     * @brief Script execution result.
     * @return Exit code on success or an Error. Meaningful once finished().
     */
    Result<int> result() const;

private:
    friend class ThreadTree;
    explicit ScriptThread(std::string path);
    void start(Runtime& runtime, std::string specifier, std::string source,
        std::function<void(bool success, const Error& error)> onExit);

    std::string path_;
    std::thread thread_;
    std::atomic<bool> cancel_{false};
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    bool finished_ = false;
    bool succeeded_ = false;
    int exitCode_ = 0;
    Error error_;
};

/**
 * @brief Registry of addressable script-thread nodes.
 *
 * ThreadTree stores nodes by path and routes messages to destination
 * mailboxes. This keeps cross-thread communication explicit: callers send to a
 * path instead of directly calling into another isolate or script context.
 *
 * Beyond one-way routing, the tree spawns script threads, carries request/reply
 * exchanges over the same transport, and shuts everything down deterministically.
 *
 * @code{.cpp}
 * wl2::ThreadTree tree;
 * tree.create("/main/worker");
 *
 * // One-way send.
 * tree.send({.source = "/main", .destination = "/main/worker",
 *            .type = "decode", .payload = "frame-42"});
 *
 * // Request/reply.
 * auto pending = tree.request({.source = "/main", .destination = "/main/worker",
 *                              .type = "work", .payload = "input"},
 *                             std::chrono::milliseconds{1000});
 *
 * if (auto req = tree.take("/main/worker", std::chrono::milliseconds{100})) {
 *     req->reply(req->payload() + "-done");
 * }
 * wl2::ThreadReply reply = pending.wait();
 * @endcode
 */
class ThreadTree {
public:
    /// Create a tree containing the root `/main` node.
    ThreadTree();

    /// Join and clear all script threads, then close mailboxes.
    ~ThreadTree();

    ThreadTree(const ThreadTree&) = delete;
    ThreadTree& operator=(const ThreadTree&) = delete;

    /**
     * @brief Create or replace a node at path.
     * @param path Absolute node path.
     * @return Shared pointer to the created node.
     */
    std::shared_ptr<ThreadNode> create(std::string path);

    /**
     * @brief Find a node by path.
     * @param path Absolute node path.
     * @return Shared pointer to the node when found, otherwise null.
     */
    std::shared_ptr<ThreadNode> find(std::string_view path) const;

    /**
     * @brief Remove a node by path.
     * @param path Absolute node path.
     * @return True when a node was removed.
     */
    bool remove(std::string_view path);

    /**
     * @brief Route a one-way message to its destination mailbox.
     * @param message Message whose destination field names the target node.
     * @return True when the destination node exists and the message was posted.
     */
    bool send(Message message);

    /**
     * @brief Spawn a script thread at options.path running the given script.
     *
     * The script source is resolved on the calling thread; the script then runs
     * on its own operating-system thread with an independent JS engine. The new
     * thread is registered as a node so it can be addressed by path.
     *
     * @param runtime Host runtime providing resources and native modules.
     * @param options Launch options.
     * @return The script thread on success, or an Error when the script cannot
     * be resolved or the path is invalid.
     *
     * @code{.cpp}
     * wl2::ScriptThreadOptions opts;
     * opts.path = "/main/worker";
     * opts.script = "wl2:/workers/worker.js";
     * auto worker = runtime.threadTree().spawn(runtime, opts);
     * @endcode
     */
    Result<std::shared_ptr<ScriptThread>> spawn(Runtime& runtime, ScriptThreadOptions options);

    /**
     * @brief Send a request and obtain a caller-side PendingReply.
     * @param message Request message; its destination names the responder.
     * @param timeout Reply deadline measured from now.
     * @return A PendingReply that resolves to the reply, a timeout, or an
     * unreachable-destination outcome.
     */
    PendingReply request(Message message, std::chrono::milliseconds timeout);

    /**
     * @brief Take the next inbound message at path as a ThreadRequest.
     * @param path Node path to read from.
     * @param timeout Maximum time to wait for a message.
     * @return The wrapped message, or std::nullopt on timeout or unknown path.
     */
    std::optional<ThreadRequest> take(std::string_view path, std::chrono::milliseconds timeout);

    /**
     * @brief Reject every pending request addressed to a path.
     * @param path Destination path whose in-flight requests should fail.
     * @param error Diagnostic message for the rejection.
     */
    void rejectPendingFor(std::string_view path, std::string error);

    /// Default grace period before shutdown forcibly interrupts script threads.
    static constexpr std::chrono::milliseconds kDefaultShutdownGrace{2000};

    /**
     * @brief Stop everything deterministically.
     *
     * Waits up to `grace` for script threads to finish on their own, then
     * interrupts any stragglers, joins them, rejects outstanding requests, and
     * closes mailboxes. A script blocked inside a native call may extend the
     * join beyond `grace`.
     *
     * @param grace Graceful window before stragglers are interrupted.
     */
    void shutdown(std::chrono::milliseconds grace = kDefaultShutdownGrace);

    /**
     * @brief Interrupt the descendant threads of a path.
     * @param path Ancestor path whose descendant threads should be interrupted.
     */
    void interruptSubtree(std::string_view path);

    /**
     * @brief Count of replies dropped because no live request matched them.
     * @return Number of late or unknown replies routed to diagnostics.
     */
    uint64_t droppedReplies() const noexcept { return droppedReplies_.load(); }

    /**
     * @brief Return sorted node paths.
     * @return Paths currently registered in the tree.
     */
    std::vector<std::string> paths() const;

private:
    friend class ThreadRequest;
    friend class PendingReply;

    void deliverReply(ThreadReply reply);
    void expirePending(uint64_t id);
    void cancelPending(uint64_t id);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ThreadNode>> nodes_;
    std::unordered_map<uint64_t, std::shared_ptr<PendingReplyState>> pending_;
    std::vector<std::shared_ptr<ScriptThread>> scriptThreads_;
    std::atomic<uint64_t> nextId_{1};
    std::atomic<uint64_t> droppedReplies_{0};
};

} // namespace wl2
