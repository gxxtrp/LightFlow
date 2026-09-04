#pragma once

#include <lightflow/core/types.hpp>
#include <lightflow/core/slab_arena.hpp>

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace lf {

/// High-performance type-erased move-only callable container.
/// Features a fixed 48-byte Small Buffer Optimization (SBO) inline buffer,
/// guaranteeing zero steady-state heap allocations. If a closure capture exceeds
/// 48 bytes, backing memory is allocated directly from the graph's SlabArena.
/// Never uses heap malloc/new. Strictly forbids std::function.
class alignas(16) MoveOnlyTask {
public:
    static constexpr usize SBO_SIZE = 48;

private:
    enum class Op : u8 {
        Move,
        Destroy
    };

    using InvokerFn = void (*)(void*) noexcept;
    using ManagerFn = void (*)(Op op, void* src, void* dst) noexcept;

    alignas(16) std::byte m_storage[SBO_SIZE]{};
    InvokerFn m_invoker{nullptr};
    ManagerFn m_manager{nullptr};

public:
    constexpr MoveOnlyTask() noexcept = default;

    ~MoveOnlyTask() noexcept {
        reset();
    }

    MoveOnlyTask(const MoveOnlyTask&) = delete;
    MoveOnlyTask& operator=(const MoveOnlyTask&) = delete;

    MoveOnlyTask(MoveOnlyTask&& other) noexcept {
        moveFrom(std::move(other));
    }

    MoveOnlyTask& operator=(MoveOnlyTask&& other) noexcept {
        if (this != &other) {
            reset();
            moveFrom(std::move(other));
        }
        return *this;
    }

    /// Constructs a MoveOnlyTask from an invocable target.
    /// If the decayed callable fits in 48 bytes, it is constructed in-place into SBO storage.
    /// Otherwise, it is allocated from the provided SlabArena.
    template <typename F>
        requires (!std::is_same_v<std::decay_t<F>, MoveOnlyTask> && std::is_invocable_v<std::decay_t<F>>)
    explicit MoveOnlyTask(F&& callable, SlabArena* arena = nullptr) {
        using DecayedF = std::decay_t<F>;

        if constexpr (sizeof(DecayedF) <= SBO_SIZE && alignof(DecayedF) <= 16) {
            ::new (static_cast<void*>(m_storage)) DecayedF(std::forward<F>(callable));

            m_invoker = [](void* ptr) noexcept {
                (*static_cast<DecayedF*>(ptr))();
            };

            m_manager = [](Op op, void* src, void* dst) noexcept {
                auto* s = static_cast<DecayedF*>(src);
                if (op == Op::Destroy) {
                    s->~DecayedF();
                } else if (op == Op::Move) {
                    ::new (dst) DecayedF(std::move(*s));
                    s->~DecayedF();
                }
            };
        } else {
            LF_ASSERT(arena != nullptr && "Callable exceeds 48-byte SBO buffer but no SlabArena was provided");
            void* mem = arena->allocate(sizeof(DecayedF), alignof(DecayedF));
            DecayedF* obj = ::new (mem) DecayedF(std::forward<F>(callable));
            *reinterpret_cast<DecayedF**>(m_storage) = obj;

            m_invoker = [](void* ptr) noexcept {
                auto* objPtr = *reinterpret_cast<DecayedF**>(ptr);
                (*objPtr)();
            };

            m_manager = [](Op op, void* src, void* dst) noexcept {
                auto** s = reinterpret_cast<DecayedF**>(src);
                if (op == Op::Destroy) {
                    if (*s != nullptr) {
                        (*s)->~DecayedF();
                    }
                } else if (op == Op::Move) {
                    auto** d = reinterpret_cast<DecayedF**>(dst);
                    *d = *s;
                    *s = nullptr;
                }
            };
        }
    }

    /// Invokes the stored callable. No-op if empty.
    void invoke() const noexcept {
        if (m_invoker != nullptr) {
            m_invoker(const_cast<void*>(static_cast<const void*>(m_storage)));
        }
    }

    /// Function call operator invoking the stored callable.
    void operator()() const noexcept {
        invoke();
    }

    /// Returns true if this instance currently holds a callable.
    LF_NODISCARD explicit operator bool() const noexcept {
        return m_invoker != nullptr;
    }

    /// Returns true if this instance currently holds a callable.
    LF_NODISCARD bool valid() const noexcept {
        return m_invoker != nullptr;
    }

    /// Resets the container to empty, invoking any active destructor.
    void reset() noexcept {
        if (m_manager != nullptr) {
            m_manager(Op::Destroy, static_cast<void*>(m_storage), nullptr);
            m_invoker = nullptr;
            m_manager = nullptr;
        }
    }

private:
    void moveFrom(MoveOnlyTask&& other) noexcept {
        if (other.m_manager != nullptr) {
            other.m_manager(Op::Move, static_cast<void*>(other.m_storage), static_cast<void*>(m_storage));
            m_invoker = other.m_invoker;
            m_manager = other.m_manager;
            other.m_invoker = nullptr;
            other.m_manager = nullptr;
        }
    }
};

static_assert(sizeof(MoveOnlyTask) == 64, "MoveOnlyTask must be exactly 64 bytes");
static_assert(alignof(MoveOnlyTask) == 16, "MoveOnlyTask must be aligned to 16 bytes");

} // namespace lf
