#pragma once

/**
 * @file errors.h
 * @brief Lightweight error and result types used by the public API.
 */

#include <string>
#include <utility>

namespace wl2 {

/**
 * @brief Machine-readable error plus human-readable context.
 *
 * Error objects are returned through Result rather than thrown across module
 * or runtime boundaries. The code is intended for branching and tests; the
 * message is intended for logs and diagnostics.
 *
 * @code{.cpp}
 * wl2::Error error{"file_not_found", "Unable to open app.js"};
 * if (error) {
 *     std::cerr << error.code() << ": " << error.message() << "\n";
 * }
 * @endcode
 */
class Error {
public:
    /// Create an empty, non-error value.
    Error() = default;

    /**
     * @brief Create an error with a stable code and human-readable message.
     * @param code Stable machine-readable error code.
     * @param message Human-readable diagnostic message.
     * @param details Optional additional context (empty by default).
     */
    Error(std::string code, std::string message, std::string details = {})
        : code_(std::move(code)), message_(std::move(message)), details_(std::move(details)) {}

    /**
     * @brief Stable machine-readable error code.
     * @return The error code string, or an empty string for no error.
     */
    const std::string& code() const noexcept { return code_; }

    /**
     * @brief Human-readable diagnostic message.
     * @return The diagnostic message.
     */
    const std::string& message() const noexcept { return message_; }

    /**
     * @brief Optional multi-line diagnostic details.
     * @return Extra context such as JavaScript stack text, or an empty string.
     */
    const std::string& details() const noexcept { return details_; }

    /**
     * @brief Test whether this object contains an error.
     * @return True when code() is not empty.
     */
    explicit operator bool() const noexcept { return !code_.empty(); }

private:
    std::string code_;
    std::string message_;
    std::string details_;
};

/**
 * @brief Return type carrying either a value or an Error.
 *
 * Result keeps API boundaries explicit and avoids throwing exceptions through
 * dynamic module or scripting glue layers.
 *
 * @code{.cpp}
 * wl2::Result<int> result = runtime.runModule("app.js");
 * if (!result) {
 *     std::cerr << result.error().message() << "\n";
 *     return 1;
 * }
 * return result.value();
 * @endcode
 */
template <class T>
class Result {
public:
    /**
     * @brief Construct a successful result.
     * @param value Value to store in the result.
     */
    Result(T value) : value_(std::move(value)), ok_(true) {}

    /**
     * @brief Construct a failed result.
     * @param error Error to store in the result.
     */
    Result(Error error) : error_(std::move(error)), ok_(false) {}

    /**
     * @brief Test whether a value is available.
     * @return True for a successful result.
     */
    bool ok() const noexcept { return ok_; }

    /**
     * @brief Test whether a value is available.
     * @return True for a successful result.
     */
    explicit operator bool() const noexcept { return ok_; }

    /**
     * @brief Access the contained value.
     * @return Mutable reference to the stored value.
     */
    T& value() & { return value_; }

    /**
     * @brief Access the contained value.
     * @return Const reference to the stored value.
     */
    const T& value() const& { return value_; }

    /**
     * @brief Move the contained value out.
     * @return Rvalue reference to the stored value.
     */
    T&& value() && { return std::move(value_); }

    /**
     * @brief Access the error for a failed result.
     * @return Stored error. Meaningful only when ok() is false.
     */
    const Error& error() const noexcept { return error_; }

private:
    T value_{};
    Error error_;
    bool ok_ = false;
};

/**
 * @brief Result specialization for APIs that only report success or failure.
 *
 * @code{.cpp}
 * wl2::Result<void> ok = runtime.initialize();
 * if (!ok) {
 *     std::cerr << ok.error().message() << "\n";
 * }
 * @endcode
 */
template <>
class Result<void> {
public:
    /// Construct a successful result.
    Result() = default;

    /**
     * @brief Construct a failed result.
     * @param error Error to store in the result.
     */
    Result(Error error) : error_(std::move(error)), ok_(false) {}

    /**
     * @brief Test whether the operation succeeded.
     * @return True for a successful result.
     */
    bool ok() const noexcept { return ok_; }

    /**
     * @brief Test whether the operation succeeded.
     * @return True for a successful result.
     */
    explicit operator bool() const noexcept { return ok_; }

    /**
     * @brief Access the error for a failed result.
     * @return Stored error. Meaningful only when ok() is false.
     */
    const Error& error() const noexcept { return error_; }

private:
    Error error_;
    bool ok_ = true;
};

} // namespace wl2
