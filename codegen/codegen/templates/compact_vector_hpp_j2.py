TEMPLATE = """
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace {{schema.namespace}}
{
    /// @brief A std::vector-like list one pointer wide: size, capacity and
    /// elements live in one heap block, and an empty list allocates nothing.
    /// For the list fields of a class with compact_lists=True, whose
    /// objects mostly leave most of their lists empty - a std::vector costs
    /// 24 bytes empty, this 8.
    template <class T>
    class CompactVector
    {
    public:
        using value_type = T;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using reference = T &;
        using const_reference = const T &;
        using pointer = T *;
        using const_pointer = const T *;
        using iterator = T *;
        using const_iterator = const T *;
        using reverse_iterator = std::reverse_iterator<iterator>;
        using const_reverse_iterator = std::reverse_iterator<const_iterator>;

        CompactVector() noexcept = default;
        CompactVector(std::initializer_list<T> values) { assign(values.begin(), values.end()); }
        template <std::input_iterator It>
        CompactVector(It first, It last) { assign(first, last); }
        explicit CompactVector(size_type n) { resize(n); }
        CompactVector(size_type n, const T &value) { resize(n, value); }
        CompactVector(const std::vector<T> &values) { assign(values.begin(), values.end()); }
        CompactVector(std::vector<T> &&values) { assign(std::make_move_iterator(values.begin()), std::make_move_iterator(values.end())); }
        CompactVector(const CompactVector &other) { assign(other.begin(), other.end()); }
        CompactVector(CompactVector &&other) noexcept : block_(std::exchange(other.block_, nullptr)) {}
        ~CompactVector() { release(); }

        CompactVector &operator=(const CompactVector &other)
        {
            if (this != &other)
                assign(other.begin(), other.end());
            return *this;
        }
        CompactVector &operator=(CompactVector &&other) noexcept
        {
            if (this != &other)
            {
                release();
                block_ = std::exchange(other.block_, nullptr);
            }
            return *this;
        }
        CompactVector &operator=(std::initializer_list<T> values)
        {
            assign(values.begin(), values.end());
            return *this;
        }
        CompactVector &operator=(const std::vector<T> &values)
        {
            assign(values.begin(), values.end());
            return *this;
        }
        CompactVector &operator=(std::vector<T> &&values)
        {
            assign(std::make_move_iterator(values.begin()), std::make_move_iterator(values.end()));
            return *this;
        }

        /// @brief The elements as a std::vector (a copy).
        std::vector<T> to_vector() const { return std::vector<T>(begin(), end()); }
        operator std::span<const T>() const noexcept { return {data(), size()}; }
        operator std::span<T>() noexcept { return {data(), size()}; }

        template <std::input_iterator It>
        void assign(It first, It last)
        {
            clear();
            if constexpr (std::forward_iterator<It>)
                reserve(static_cast<size_type>(std::distance(first, last)));
            for (; first != last; ++first)
                emplace_back(*first);
        }

        size_type size() const noexcept { return block_ ? block_->size : 0; }
        size_type capacity() const noexcept { return block_ ? block_->capacity : 0; }
        bool empty() const noexcept { return size() == 0; }

        T *data() noexcept { return block_ ? elements(block_) : nullptr; }
        const T *data() const noexcept { return block_ ? elements(block_) : nullptr; }
        iterator begin() noexcept { return data(); }
        iterator end() noexcept { return data() + size(); }
        const_iterator begin() const noexcept { return data(); }
        const_iterator end() const noexcept { return data() + size(); }
        const_iterator cbegin() const noexcept { return begin(); }
        const_iterator cend() const noexcept { return end(); }
        reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
        reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
        const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
        const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }

        T &operator[](size_type i) noexcept { return data()[i]; }
        const T &operator[](size_type i) const noexcept { return data()[i]; }
        T &front() noexcept { return data()[0]; }
        const T &front() const noexcept { return data()[0]; }
        T &back() noexcept { return data()[size() - 1]; }
        const T &back() const noexcept { return data()[size() - 1]; }

        void reserve(size_type n)
        {
            if (n > capacity())
                reallocate(n);
        }
        void shrink_to_fit()
        {
            if (empty())
                release();
            else if (capacity() > size())
                reallocate(size());
        }
        void clear() noexcept
        {
            if (block_)
            {
                std::destroy_n(elements(block_), block_->size);
                block_->size = 0;
            }
        }

        template <class... Args>
        T &emplace_back(Args &&...args)
        {
            if (size() == capacity())
                reallocate(grown(size() + 1));
            T *slot = elements(block_) + block_->size;
            std::construct_at(slot, std::forward<Args>(args)...);
            ++block_->size;
            return *slot;
        }
        void push_back(const T &value) { emplace_back(value); }
        void push_back(T &&value) { emplace_back(std::move(value)); }
        void pop_back() noexcept
        {
            std::destroy_at(elements(block_) + block_->size - 1);
            --block_->size;
        }

        void resize(size_type n)
        {
            while (size() > n)
                pop_back();
            reserve(n);
            while (size() < n)
                emplace_back();
        }
        void resize(size_type n, const T &value)
        {
            while (size() > n)
                pop_back();
            reserve(n);
            while (size() < n)
                emplace_back(value);
        }

        template <class... Args>
        iterator emplace(const_iterator position, Args &&...args)
        {
            const size_type index = static_cast<size_type>(position - begin());
            emplace_back(std::forward<Args>(args)...);
            std::rotate(begin() + index, end() - 1, end());
            return begin() + index;
        }
        iterator insert(const_iterator position, const T &value) { return emplace(position, value); }
        iterator insert(const_iterator position, T &&value) { return emplace(position, std::move(value)); }
        template <std::input_iterator It>
        iterator insert(const_iterator position, It first, It last)
        {
            const size_type index = static_cast<size_type>(position - begin());
            const size_type old_size = size();
            for (; first != last; ++first)
                emplace_back(*first);
            std::rotate(begin() + index, begin() + old_size, end());
            return begin() + index;
        }
        iterator erase(const_iterator first, const_iterator last)
        {
            const size_type index = static_cast<size_type>(first - begin());
            const size_type count = static_cast<size_type>(last - first);
            if (count > 0)
            {
                std::move(begin() + index + count, end(), begin() + index);
                for (size_type i = 0; i < count; ++i)
                    pop_back();
            }
            return begin() + index;
        }
        iterator erase(const_iterator position) { return erase(position, position + 1); }

        void swap(CompactVector &other) noexcept { std::swap(block_, other.block_); }

        friend bool operator==(const CompactVector &a, const CompactVector &b)
        {
            return std::equal(a.begin(), a.end(), b.begin(), b.end());
        }
        friend bool operator==(const CompactVector &a, const std::vector<T> &b)
        {
            return std::equal(a.begin(), a.end(), b.begin(), b.end());
        }

    private:
        struct Block
        {
            uint32_t size;
            uint32_t capacity;
        };
        static constexpr std::size_t kAlign = std::max(alignof(Block), alignof(T));
        static constexpr std::size_t kOffset = (sizeof(Block) + kAlign - 1) / kAlign * kAlign;

        static T *elements(Block *block) noexcept { return reinterpret_cast<T *>(reinterpret_cast<std::byte *>(block) + kOffset); }
        static const T *elements(const Block *block) noexcept
        {
            return reinterpret_cast<const T *>(reinterpret_cast<const std::byte *>(block) + kOffset);
        }
        size_type grown(size_type needed) const noexcept { return std::max(needed, capacity() * 2); }

        void reallocate(size_type new_capacity)
        {
            void *memory = ::operator new(kOffset + new_capacity * sizeof(T), std::align_val_t{kAlign});
            Block *block = ::new (memory) Block{0, static_cast<uint32_t>(new_capacity)};
            if (block_)
            {
                std::uninitialized_move_n(elements(block_), block_->size, elements(block));
                block->size = block_->size;
            }
            release();
            block_ = block;
        }
        void release() noexcept
        {
            if (!block_)
                return;
            std::destroy_n(elements(block_), block_->size);
            ::operator delete(block_, std::align_val_t{kAlign});
            block_ = nullptr;
        }

        Block *block_ = nullptr;
    };
}
"""
