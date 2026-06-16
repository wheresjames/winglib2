#include "wl2/thread_tree.h"

#include "wl2/crash_report.h"
#include "wl2/js_engine.h"
#include "wl2/runtime.h"

#include <algorithm>
#include <utility>

namespace wl2 {

/**
 * @brief Shared state behind a PendingReply and its ThreadTree registry entry.
 *
 * Liveness is owned by the ThreadTree's pending map under the tree mutex: while
 * an entry is present the request is unresolved, and every terminal transition
 * removes it under that mutex so exactly one transition wins. The terminal
 * fields are published under this state's mutex so waiters read them safely.
 */
struct PendingReplyState {
    std::mutex mutex;
    std::condition_variable cv;
    uint64_t id = 0;
    std::string destination;
    std::chrono::steady_clock::time_point deadline;
    bool terminal = false;
    ThreadReply reply;
    ThreadTree* tree = nullptr;
};

namespace {

thread_local std::string tlsThreadPath = "/main";

// Parent path of an absolute tree path: "/main/worker" -> "/main". Returns an
// empty string when there is no parent segment.
std::string parent_path(const std::string& path) {
    auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return {};
    }
    return path.substr(0, slash);
}

std::optional<std::string> validate_thread_path(std::string_view path) {
    if (path.empty()) {
        return "path must not be empty";
    }
    if (path.front() != '/') {
        return "path must be absolute";
    }
    if (path.size() > 1 && path.back() == '/') {
        return "path must not have a trailing slash";
    }
    size_t segmentStart = 1;
    while (segmentStart <= path.size()) {
        const size_t slash = path.find('/', segmentStart);
        const auto segment = path.substr(segmentStart,
            slash == std::string_view::npos ? std::string_view::npos : slash - segmentStart);
        if (segment.empty()) {
            return "path must not contain empty segments";
        }
        if (segment == "." || segment == "..") {
            return "path must not contain dot segments";
        }
        if (slash == std::string_view::npos) {
            break;
        }
        segmentStart = slash + 1;
    }
    return std::nullopt;
}

ThreadReply make_terminal(ReplyStatus status, uint64_t id, std::string source, std::string error) {
    ThreadReply reply;
    reply.status = status;
    reply.requestId = id;
    reply.source = std::move(source);
    reply.error = std::move(error);
    return reply;
}

// Publish a terminal reply to a state that has already been removed from the
// tree's pending map. The caller owns the only live reference, so this is the
// single transition for that state.
void resolve_state(const std::shared_ptr<PendingReplyState>& state, ThreadReply reply) {
    {
        std::lock_guard lock(state->mutex);
        if (state->terminal) {
            return;
        }
        state->terminal = true;
        state->reply = std::move(reply);
    }
    state->cv.notify_all();
}

} // namespace

const std::string& currentThreadPath() {
    return tlsThreadPath;
}

ScopedThreadPath::ScopedThreadPath(std::string path)
    : previous_(std::move(tlsThreadPath)) {
    tlsThreadPath = std::move(path);
}

ScopedThreadPath::~ScopedThreadPath() {
    tlsThreadPath = std::move(previous_);
}

void Mailbox::post(Message message) {
    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return;
        }
        messages_.push_back(std::move(message));
    }
    cv_.notify_one();
}

std::optional<Message> Mailbox::wait(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, timeout, [&] { return closed_ || !messages_.empty(); });
    if (messages_.empty()) {
        return std::nullopt;
    }
    auto message = std::move(messages_.front());
    messages_.pop_front();
    return message;
}

size_t Mailbox::size() const {
    std::lock_guard lock(mutex_);
    return messages_.size();
}

void Mailbox::close() {
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }
    cv_.notify_all();
}

ThreadNode::ThreadNode(std::string path)
    : path_(std::move(path)) {}

// --- PendingReply ----------------------------------------------------------

PendingReply::PendingReply(std::shared_ptr<PendingReplyState> state)
    : state_(std::move(state)) {}

uint64_t PendingReply::id() const noexcept {
    return state_ ? state_->id : 0;
}

bool PendingReply::done() const {
    if (!state_) {
        return true;
    }
    std::lock_guard lock(state_->mutex);
    return state_->terminal;
}

std::optional<ThreadReply> PendingReply::poll() const {
    if (!state_) {
        return std::nullopt;
    }
    {
        std::lock_guard lock(state_->mutex);
        if (state_->terminal) {
            return state_->reply;
        }
        if (std::chrono::steady_clock::now() < state_->deadline) {
            return std::nullopt;
        }
    }
    state_->tree->expirePending(state_->id);
    std::lock_guard lock(state_->mutex);
    return state_->reply;
}

std::optional<ThreadReply> PendingReply::wait(std::chrono::milliseconds timeout) const {
    if (!state_) {
        return std::nullopt;
    }
    const auto callerUntil = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(state_->mutex);
    while (!state_->terminal) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= state_->deadline) {
            lock.unlock();
            state_->tree->expirePending(state_->id);
            lock.lock();
            break;
        }
        if (now >= callerUntil) {
            return std::nullopt;
        }
        state_->cv.wait_until(lock, std::min(callerUntil, state_->deadline));
    }
    return state_->reply;
}

ThreadReply PendingReply::wait() const {
    if (!state_) {
        return make_terminal(ReplyStatus::Cancelled, 0, "", "empty pending reply");
    }
    std::unique_lock lock(state_->mutex);
    while (!state_->terminal) {
        if (std::chrono::steady_clock::now() >= state_->deadline) {
            lock.unlock();
            state_->tree->expirePending(state_->id);
            lock.lock();
            break;
        }
        state_->cv.wait_until(lock, state_->deadline);
    }
    return state_->reply;
}

void PendingReply::cancel() {
    if (state_) {
        state_->tree->cancelPending(state_->id);
    }
}

// --- ThreadRequest ---------------------------------------------------------

ThreadRequest::ThreadRequest(ThreadTree* tree, Message message)
    : tree_(tree), message_(std::move(message)) {}

ThreadRequest::ThreadRequest(ThreadRequest&& other) noexcept
    : tree_(other.tree_),
      message_(std::move(other.message_)),
      resolved_(other.resolved_),
      deferred_(other.deferred_) {
    other.tree_ = nullptr;
}

ThreadRequest& ThreadRequest::operator=(ThreadRequest&& other) noexcept {
    if (this != &other) {
        // Resolve any obligation this request still owns before overwriting it.
        if (tree_ && message_.expectsReply && !resolved_ && !deferred_) {
            reject("request_dropped");
        }
        tree_ = other.tree_;
        message_ = std::move(other.message_);
        resolved_ = other.resolved_;
        deferred_ = other.deferred_;
        other.tree_ = nullptr;
    }
    return *this;
}

ThreadRequest::~ThreadRequest() {
    if (tree_ && message_.expectsReply && !resolved_ && !deferred_) {
        reject("request_dropped");
    }
}

bool ThreadRequest::reply(std::string payload, std::string type) {
    if (!tree_ || !message_.expectsReply || resolved_) {
        return false;
    }
    resolved_ = true;
    ThreadReply reply;
    reply.status = ReplyStatus::Ok;
    reply.requestId = message_.id;
    reply.source = message_.destination;
    reply.type = std::move(type);
    reply.payload = std::move(payload);
    tree_->deliverReply(std::move(reply));
    return true;
}

bool ThreadRequest::reject(std::string error) {
    if (!tree_ || !message_.expectsReply || resolved_) {
        return false;
    }
    resolved_ = true;
    tree_->deliverReply(make_terminal(ReplyStatus::Rejected, message_.id, message_.destination, std::move(error)));
    return true;
}

// --- ScriptThread ----------------------------------------------------------

ScriptThread::ScriptThread(std::string path)
    : path_(std::move(path)) {}

ScriptThread::~ScriptThread() {
    join();
}

void ScriptThread::start(Runtime& runtime, std::string specifier, std::string source,
    std::function<void(bool, const Error&)> onExit) {
    thread_ = std::thread([this, &runtime, specifier = std::move(specifier),
                              source = std::move(source), onExit = std::move(onExit)]() mutable {
        ScopedThreadPath threadPath(path_);
        // Register this script thread so it is listed in any crash report. The
        // path_ string outlives the thread, so its pointer stays valid until the
        // matching unregister below.
        wl2::crash::registerThread(path_.c_str());
        auto engine = createConfiguredJsEngine();
        Result<int> result = engine->runModule(runtime, specifier, source, &cancel_);
        bool success = false;
        Error error;
        {
            std::lock_guard lock(mutex_);
            if (result) {
                succeeded_ = true;
                exitCode_ = result.value();
            } else {
                succeeded_ = false;
                error_ = result.error();
            }
            finished_ = true;
            success = succeeded_;
            error = error_;
        }
        cv_.notify_all();
        if (onExit) {
            onExit(success, error);
        }
        wl2::crash::unregisterThread(path_.c_str());
    });
}

void ScriptThread::interrupt() {
    cancel_.store(true, std::memory_order_relaxed);
}

bool ScriptThread::finished() const {
    std::lock_guard lock(mutex_);
    return finished_;
}

bool ScriptThread::failed() const {
    std::lock_guard lock(mutex_);
    return finished_ && !succeeded_;
}

bool ScriptThread::wait(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, timeout, [&] { return finished_; });
    return finished_;
}

void ScriptThread::join() {
    if (thread_.joinable() && !isCurrentThread()) {
        thread_.join();
    }
}

bool ScriptThread::isCurrentThread() const noexcept {
    return thread_.joinable() && thread_.get_id() == std::this_thread::get_id();
}

Result<int> ScriptThread::result() const {
    std::lock_guard lock(mutex_);
    if (succeeded_) {
        return exitCode_;
    }
    return error_;
}

// --- ThreadTree ------------------------------------------------------------

ThreadTree::ThreadTree() {
    nodes_.emplace("/main", std::make_shared<ThreadNode>("/main"));
}

ThreadTree::~ThreadTree() {
    shutdown();
}

std::shared_ptr<ThreadNode> ThreadTree::create(std::string path) {
    std::lock_guard lock(mutex_);
    auto [it, inserted] = nodes_.emplace(path, std::make_shared<ThreadNode>(path));
    return it->second;
}

std::shared_ptr<ThreadNode> ThreadTree::find(std::string_view path) const {
    std::lock_guard lock(mutex_);
    auto it = nodes_.find(std::string(path));
    return it == nodes_.end() ? nullptr : it->second;
}

bool ThreadTree::remove(std::string_view path) {
    if (path == "/main") {
        return false;
    }
    std::shared_ptr<ThreadNode> node;
    {
        std::lock_guard lock(mutex_);
        auto it = nodes_.find(std::string(path));
        if (it == nodes_.end()) {
            return false;
        }
        node = it->second;
        nodes_.erase(it);
    }
    node->mailbox().close();
    return true;
}

bool ThreadTree::send(Message message) {
    auto node = find(message.destination);
    if (!node) {
        return false;
    }
    node->mailbox().post(std::move(message));
    return true;
}

Result<std::shared_ptr<ScriptThread>> ThreadTree::spawn(Runtime& runtime, ScriptThreadOptions options) {
    if (auto invalid = validate_thread_path(options.path)) {
        return Error("thread_invalid_path", "ScriptThreadOptions.path " + *invalid);
    }
    if (options.path == "/main") {
        return Error("thread_invalid_path", "ScriptThreadOptions.path must not be /main");
    }

    std::string source = std::move(options.source);
    std::string specifier = options.script.empty() ? options.path : options.script;
    if (source.empty()) {
        if (options.script.empty()) {
            return Error("thread_no_script", "ScriptThreadOptions needs a script specifier or inline source");
        }
        auto loaded = runtime.loadTextResource(options.script);
        if (!loaded) {
            return loaded.error();
        }
        source = std::move(loaded.value());
    }

    // Static native modules must be registered before a child imports them.
    if (auto init = runtime.initialize(); !init) {
        return init.error();
    }

    auto thread = std::shared_ptr<ScriptThread>(new ScriptThread(options.path));
    {
        std::lock_guard lock(mutex_);
        if (nodes_.find(options.path) != nodes_.end()) {
            return Error("thread_path_exists", "Thread path already exists: " + options.path);
        }
        nodes_.emplace(options.path, std::make_shared<ThreadNode>(options.path));
        scriptThreads_.push_back(thread);
    }

    const std::string path = options.path;
    const std::string parent = parent_path(path);
    const bool reportExit = options.reportExitToParent;
    const ThreadFailurePolicy policy = options.onFailure;
    thread->start(runtime, std::move(specifier), std::move(source),
        [this, path, parent, reportExit, policy](bool success, const Error& error) {
            // The thread is gone: no one will answer requests addressed to it.
            rejectPendingFor(path, "thread_exited");

            // Report the exit to the parent over the same tree transport.
            if (reportExit && !parent.empty()) {
                Message exit;
                exit.source = path;
                exit.destination = parent;
                exit.type = success ? "thread-exit" : "thread-error";
                exit.payload = success ? "" : error.code();
                send(std::move(exit));
            }

            // Optionally stop the failed thread's descendants.
            if (!success && policy == ThreadFailurePolicy::ShutdownSubtree) {
                interruptSubtree(path);
            }
        });

    return thread;
}

PendingReply ThreadTree::request(Message message, std::chrono::milliseconds timeout) {
    auto state = std::make_shared<PendingReplyState>();
    state->id = nextId_.fetch_add(1);
    state->destination = message.destination;
    state->deadline = std::chrono::steady_clock::now() + timeout;
    state->tree = this;

    message.id = state->id;
    message.expectsReply = true;

    std::shared_ptr<ThreadNode> node;
    {
        std::lock_guard lock(mutex_);
        auto it = nodes_.find(message.destination);
        if (it != nodes_.end()) {
            node = it->second;
            pending_.emplace(state->id, state);
        }
    }

    if (!node) {
        resolve_state(state, make_terminal(ReplyStatus::Unreachable, state->id, message.destination,
            "no such destination: " + message.destination));
        return PendingReply{state};
    }

    node->mailbox().post(std::move(message));
    return PendingReply{state};
}

std::optional<ThreadRequest> ThreadTree::take(std::string_view path, std::chrono::milliseconds timeout) {
    auto node = find(path);
    if (!node) {
        return std::nullopt;
    }
    auto message = node->mailbox().wait(timeout);
    if (!message) {
        return std::nullopt;
    }
    return ThreadRequest{this, std::move(*message)};
}

void ThreadTree::rejectPendingFor(std::string_view path, std::string error) {
    std::vector<std::shared_ptr<PendingReplyState>> victims;
    {
        std::lock_guard lock(mutex_);
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second->destination == path) {
                victims.push_back(it->second);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& state : victims) {
        resolve_state(state, make_terminal(ReplyStatus::Unreachable, state->id, std::string(path), error));
    }
}

void ThreadTree::shutdown(std::chrono::milliseconds grace) {
    std::vector<std::shared_ptr<ScriptThread>> threads;
    std::vector<std::shared_ptr<ThreadNode>> nodes;
    std::vector<std::shared_ptr<PendingReplyState>> pending;
    {
        std::lock_guard lock(mutex_);
        for (auto it = scriptThreads_.begin(); it != scriptThreads_.end();) {
            if ((*it)->isCurrentThread()) {
                ++it;
            } else {
                threads.push_back(*it);
                it = scriptThreads_.erase(it);
            }
        }
        for (auto& [path, node] : nodes_) {
            nodes.push_back(node);
        }
        for (auto& [id, state] : pending_) {
            pending.push_back(state);
        }
        pending_.clear();
    }

    // Graceful shutdown pass: give threads up to `grace` total to finish on their own.
    const auto deadline = std::chrono::steady_clock::now() + grace;
    for (auto& thread : threads) {
        if (thread->isCurrentThread()) {
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = now < deadline
            ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            : std::chrono::milliseconds{0};
        thread->wait(remaining);
    }

    // Forced shutdown pass: interrupt any stragglers, then join everything.
    for (auto& thread : threads) {
        if (!thread->finished()) {
            thread->interrupt();
        }
    }
    for (auto& thread : threads) {
        if (!thread->isCurrentThread()) {
            thread->join();
        }
    }

    for (auto& node : nodes) {
        node->mailbox().close();
    }
    for (auto& state : pending) {
        resolve_state(state, make_terminal(ReplyStatus::Unreachable, state->id, state->destination,
            "thread tree shut down"));
    }
}

void ThreadTree::interruptSubtree(std::string_view path) {
    const std::string prefix = std::string(path) + "/";
    std::vector<std::shared_ptr<ScriptThread>> descendants;
    {
        std::lock_guard lock(mutex_);
        for (auto& thread : scriptThreads_) {
            if (thread->path().rfind(prefix, 0) == 0) {
                descendants.push_back(thread);
            }
        }
    }
    for (auto& thread : descendants) {
        thread->interrupt();
    }
}

std::vector<std::string> ThreadTree::paths() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    out.reserve(nodes_.size());
    for (const auto& [path, _] : nodes_) {
        out.push_back(path);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void ThreadTree::deliverReply(ThreadReply reply) {
    std::shared_ptr<PendingReplyState> state;
    {
        std::lock_guard lock(mutex_);
        auto it = pending_.find(reply.requestId);
        if (it != pending_.end()) {
            state = it->second;
            pending_.erase(it);
        }
    }
    if (!state) {
        // The request already timed out, was cancelled, or never existed.
        droppedReplies_.fetch_add(1);
        return;
    }
    resolve_state(state, std::move(reply));
}

void ThreadTree::expirePending(uint64_t id) {
    std::shared_ptr<PendingReplyState> state;
    {
        std::lock_guard lock(mutex_);
        auto it = pending_.find(id);
        if (it == pending_.end()) {
            return;
        }
        state = it->second;
        pending_.erase(it);
    }
    resolve_state(state, make_terminal(ReplyStatus::Timeout, id, state->destination, "request timed out"));
}

void ThreadTree::cancelPending(uint64_t id) {
    std::shared_ptr<PendingReplyState> state;
    {
        std::lock_guard lock(mutex_);
        auto it = pending_.find(id);
        if (it == pending_.end()) {
            return;
        }
        state = it->second;
        pending_.erase(it);
    }
    resolve_state(state, make_terminal(ReplyStatus::Cancelled, id, state->destination, "request cancelled"));
}

} // namespace wl2
