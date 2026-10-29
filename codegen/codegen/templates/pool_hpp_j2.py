TEMPLATE = """
#pragma once
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace {{schema.namespace}}
{
    template <class T, class IdT>
    class Pool {
    public:
        struct Slot {
            T value{};
            uint32_t generation = 0;
            bool alive = false;
            bool retired = false; // dead, but held for restore() - off the free list
        };

        template <class... Args>
        IdT create(Args&&... args) {
            uint32_t idx;
            if (!free_.empty()) {
                idx = free_.back();
                free_.pop_back();
                Slot& s = slots_[idx];
                s.value = T{std::forward<Args>(args)...};
                s.alive = true;
                return IdT{idx, s.generation};
            }

            idx = static_cast<uint32_t>(slots_.size());
            Slot s;
            s.value = T{std::forward<Args>(args)...};
            s.alive = true;
            slots_.push_back(std::move(s));
            return IdT{idx, 0};
        }

        bool erase(IdT id) {
            if (auto* s = try_slot(id)) {
                s->value = T{};
                s->alive = false;
                ++s->generation;
                free_.push_back(id.index);
                return true;
            }
            return false;
        }

        /// @brief Kill `id` but keep its slot - same generation, off the
        /// free list - so restore() can bring it back at the same id.
        /// Returns the object's value, or nullopt if `id` isn't live.
        std::optional<T> retire(IdT id) {
            auto* s = try_slot(id);
            if (!s) return std::nullopt;
            std::optional<T> value(std::move(s->value));
            s->value = T{};
            s->alive = false;
            s->retired = true;
            ++retired_count_;
            return value;
        }

        /// @brief Bring a retire()d `id` back to life holding `value`.
        /// False if `id` isn't retired.
        bool restore(IdT id, T&& value) {
            if (!is_retired(id)) return false;
            Slot& s = slots_[id.index];
            s.value = std::move(value);
            s.alive = true;
            s.retired = false;
            --retired_count_;
            return true;
        }

        /// @brief Give a retire()d `id`'s slot back for reuse, under a new
        /// generation. A no-op if `id` isn't retired.
        void release(IdT id) {
            if (!is_retired(id)) return;
            Slot& s = slots_[id.index];
            s.retired = false;
            ++s.generation;
            --retired_count_;
            free_.push_back(id.index);
        }

        bool is_retired(IdT id) const {
            return id.index < slots_.size() && slots_[id.index].retired && slots_[id.index].generation == id.generation;
        }

        T* get(IdT id) {
            if (auto* s = try_slot(id)) return &s->value;
            return nullptr;
        }

        const T* get(IdT id) const {
            if (auto* s = try_slot(id)) return &s->value;
            return nullptr;
        }

        bool contains(IdT id) const {
            return try_slot(id) != nullptr;
        }

        const std::vector<Slot>& slots() const noexcept { return slots_; }
        std::vector<Slot>& slots() noexcept { return slots_; }

        template <typename Fn>
        void for_each_id(Fn&& fn) const {
            for (uint32_t i = 0; i < slots_.size(); ++i) {
                if (slots_[i].alive) {
                    fn(IdT{ i, slots_[i].generation });
                }
            }
        }

        std::vector<IdT> ids() const {
            std::vector<IdT> result;
            result.reserve(slots_.size());

            for (uint32_t i = 0; i < slots_.size(); ++i) {
                if (slots_[i].alive) {
                    result.push_back(IdT{ i, slots_[i].generation });
                }
            }
            return result;
        }

        bool is_empty() {
            return slots_.size() == 0;
        }

        uint64_t size() {
            return slots_.size();
        }

        // Also drops the free list - it holds slot indices, and create()
        // would otherwise pop one past the end of the now-empty slots_.
        void clear() {
            slots_.clear();
            free_.clear();
            retired_count_ = 0;
        }

        /// @brief Replace the whole pool with `values`, alive at indices
        /// 0..n-1 with generation 0 - so row i becomes Id{i, 0}. For the
        /// native file format's loader (src/io/native_format.cpp) only; indexes
        /// are the caller's to rebuild (Root::rebuild_indexes()).
        void load_dense(std::vector<T> values) {
            slots_.clear();
            free_.clear();
            retired_count_ = 0;
            slots_.resize(values.size());
            for (size_t i = 0; i < values.size(); ++i) {
                slots_[i].value = std::move(values[i]);
                slots_[i].alive = true;
            }
        }

        /// @brief load_dense() with `n` value-initialized objects, built in
        /// place (no temporary to move from) - the loader then fills them.
        void load_dense(size_t n) {
            slots_.clear();
            free_.clear();
            retired_count_ = 0;
            slots_.resize(n);
            for (Slot &slot : slots_)
                slot.alive = true;
        }

        /// @brief Number of live objects (size() counts every slot).
        size_t alive_count() const noexcept {
            return slots_.size() - free_.size() - retired_count_;
        }

    private:
        Slot* try_slot(IdT id) {
            if (id.index >= slots_.size()) return nullptr;
            Slot& s = slots_[id.index];
            if (!s.alive || s.generation != id.generation) return nullptr;
            return &s;
        }

        const Slot* try_slot(IdT id) const {
            if (id.index >= slots_.size()) return nullptr;
            const Slot& s = slots_[id.index];
            if (!s.alive || s.generation != id.generation) return nullptr;
            return &s;
        }

        std::vector<Slot> slots_;
        std::vector<uint32_t> free_;
        size_t retired_count_ = 0;
    };
}
"""
