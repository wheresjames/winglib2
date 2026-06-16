#include "wl2/buffer.h"

#include <algorithm>
#include <cstring>

namespace wl2 {

Buffer::Buffer()
    : storage_(std::make_shared<std::vector<std::byte>>()) {}

Buffer::Buffer(std::shared_ptr<std::vector<std::byte>> storage, size_t offset, size_t size)
    : storage_(std::move(storage)), offset_(offset), size_(size) {}

Buffer Buffer::copy(std::span<const std::byte> bytes) {
    auto storage = std::make_shared<std::vector<std::byte>>(bytes.begin(), bytes.end());
    return Buffer(std::move(storage), 0, bytes.size());
}

Buffer Buffer::fromString(std::string_view text) {
    auto storage = std::make_shared<std::vector<std::byte>>(text.size());
    std::memcpy(storage->data(), text.data(), text.size());
    return Buffer(std::move(storage), 0, text.size());
}

size_t Buffer::size() const noexcept {
    return size_;
}

bool Buffer::empty() const noexcept {
    return size_ == 0;
}

std::span<const std::byte> Buffer::read() const noexcept {
    return {storage_->data() + offset_, size_};
}

std::span<std::byte> Buffer::mutableView() {
    detach();
    return {storage_->data(), size_};
}

Buffer Buffer::slice(size_t offset, size_t length) const {
    if (offset >= size_) {
        return {};
    }
    const auto bounded = std::min(length, size_ - offset);
    return Buffer(storage_, offset_ + offset, bounded);
}

std::string Buffer::text() const {
    const auto view = read();
    return {reinterpret_cast<const char*>(view.data()), view.size()};
}

void Buffer::detach() {
    if (!storage_.unique() || offset_ != 0 || storage_->size() != size_) {
        auto view = read();
        auto next = std::make_shared<std::vector<std::byte>>(view.begin(), view.end());
        storage_ = std::move(next);
        offset_ = 0;
    }
}

SharedBuffer::SharedBuffer(std::string name, size_t size, bool writable)
    : name_(std::move(name)), size_(size), writable_(writable) {}

} // namespace wl2
