#pragma once

#include "roadmaker/road/id.hpp"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace roadmaker {

/// Slot-map arena: stable generational handles over contiguous storage.
///
/// Erasing a slot bumps its generation, so handles held elsewhere become
/// stale rather than dangling — `get` on a stale handle returns nullptr.
/// Pointers returned by `get` are invalidated by any mutation of the arena;
/// never store them across mutations (hold the Id instead).
template <class T, class IdT>
class Arena {
public:
  /// Constructs a new element in place. Reuses erased slots (with a fresh
  /// generation) before growing.
  template <class... Args>
  IdT emplace(Args&&... args) {
    std::uint32_t index = 0;
    if (free_.empty()) {
      index = static_cast<std::uint32_t>(slots_.size());
      slots_.emplace_back();
    } else {
      index = free_.back();
      free_.pop_back();
    }
    Slot& slot = slots_[index];
    slot.value.emplace(std::forward<Args>(args)...);
    ++alive_;
    return IdT{.index = index, .gen = slot.gen};
  }

  /// nullptr if `id` is invalid, stale, or out of range.
  [[nodiscard]] T* get(IdT id) { return const_cast<T*>(std::as_const(*this).get(id)); }

  [[nodiscard]] const T* get(IdT id) const {
    if (id.index >= slots_.size()) {
      return nullptr;
    }
    const Slot& slot = slots_[id.index];
    if (slot.gen != id.gen || !slot.value.has_value()) {
      return nullptr;
    }
    return &*slot.value;
  }

  /// Destroys the element and invalidates its handle. Returns false if the
  /// handle was already invalid or stale.
  bool erase(IdT id) {
    if (get(id) == nullptr) {
      return false;
    }
    Slot& slot = slots_[id.index];
    slot.value.reset();
    ++slot.gen;
    free_.push_back(id.index);
    --alive_;
    return true;
  }

  [[nodiscard]] std::size_t size() const { return alive_; }

  [[nodiscard]] bool empty() const { return alive_ == 0; }

  /// Visits every live element in slot order (insertion order until slots
  /// are reused). `fn` is called as fn(IdT, T&). Must not mutate the arena.
  template <class Fn>
  void for_each(Fn fn) {
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].value.has_value()) {
        fn(IdT{.index = i, .gen = slots_[i].gen}, *slots_[i].value);
      }
    }
  }

  template <class Fn>
  void for_each(Fn fn) const {
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].value.has_value()) {
        fn(IdT{.index = i, .gen = slots_[i].gen}, *slots_[i].value);
      }
    }
  }

private:
  struct Slot {
    std::optional<T> value;
    std::uint32_t gen = 0;
  };

  std::vector<Slot> slots_;
  std::vector<std::uint32_t> free_;
  std::size_t alive_ = 0;
};

} // namespace roadmaker
