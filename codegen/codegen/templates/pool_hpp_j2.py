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
                s->alive = false;
                ++s->generation;
                free_.push_back(id.index);
                return true;
            }
            return false;
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

        void clear() {
            slots_.clear();
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
    };
}
"""
