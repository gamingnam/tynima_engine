#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

// A byte array that is made and freed in one step. std::vector<uint8_t>
// value-initialises every byte a resize adds and destroys every byte it
// holds one at a time — trivially, but in an unoptimised build the loop is
// still run, and for a texture's worth of pixels that is a third of a
// second at each end. A file's bytes and a mip chain go here instead: the
// memory is left as the allocator gives it, and returned as one block.
namespace tynima::core {

class Bytes {
public:
    Bytes() = default;
    // `size` bytes, uninitialised.
    explicit Bytes(std::size_t size) { resize(size); }
    Bytes(const void* data, std::size_t size) { assign(data, size); }
    Bytes(const Bytes& other) { assign(other.data_.get(), other.size_); }
    Bytes(Bytes&& other) noexcept : data_(std::move(other.data_)), size_(other.size_) { other.size_ = 0; }
    Bytes& operator=(const Bytes& other) {
        if (this != &other) {
            assign(other.data_.get(), other.size_);
        }
        return *this;
    }
    Bytes& operator=(Bytes&& other) noexcept {
        data_ = std::move(other.data_);
        size_ = other.size_;
        other.size_ = 0;
        return *this;
    }
    ~Bytes() = default;

    [[nodiscard]] std::uint8_t* data() noexcept { return data_.get(); }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return data_.get(); }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::uint8_t& operator[](std::size_t i) noexcept { return data_[i]; }
    [[nodiscard]] const std::uint8_t& operator[](std::size_t i) const noexcept { return data_[i]; }
    [[nodiscard]] std::uint8_t* begin() noexcept { return data_.get(); }
    [[nodiscard]] std::uint8_t* end() noexcept { return data_.get() + size_; }
    [[nodiscard]] const std::uint8_t* begin() const noexcept { return data_.get(); }
    [[nodiscard]] const std::uint8_t* end() const noexcept { return data_.get() + size_; }

    // A copy of `size` bytes from `data`.
    void assign(const void* data, std::size_t size) {
        resize(size);
        if (size > 0) {
            std::memcpy(data_.get(), data, size);
        }
    }
    // `size` bytes: the first min(old, new) kept, the rest uninitialised.
    void resize(std::size_t size) {
        if (size == size_) {
            return;
        }
        std::unique_ptr<std::uint8_t[]> next(size > 0 ? new std::uint8_t[size] : nullptr);
        if (size > 0 && size_ > 0) {
            std::memcpy(next.get(), data_.get(), size < size_ ? size : size_);
        }
        data_ = std::move(next);
        size_ = size;
    }
    void clear() noexcept {
        data_.reset();
        size_ = 0;
    }
    [[nodiscard]] bool operator==(const Bytes& other) const noexcept {
        return size_ == other.size_ &&
               (size_ == 0 || std::memcmp(data_.get(), other.data_.get(), size_) == 0);
    }

private:
    std::unique_ptr<std::uint8_t[]> data_;
    std::size_t size_ = 0;
};

} // namespace tynima::core
