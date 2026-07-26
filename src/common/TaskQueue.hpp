#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace core
{
// Network threads push, the game thread drains inside the tick.
//
// This exists because the engine is not thread safe: touching the game from the
// network thread crashes eventually and at random, which is close to undebuggable.
// Everything coming off the wire has to land here first.
class TaskQueue
{
public:
    using Task = std::function<void()>;

    void Push(Task aTask)
    {
        if (!aTask)
        {
            return;
        }

        std::scoped_lock lock(m_mutex);
        m_pending.push_back(std::move(aTask));
    }

    // Runs tasks and returns how many ran. aBudget of 0 means everything queued at
    // the moment of the call.
    //
    // Tasks run outside the lock, so a task is free to push another one: it lands in
    // the next drain rather than deadlocking or running in this one.
    //
    // Safe to call from several threads at once. The engine dispatches its update
    // groups on a worker pool, so two of our tick callbacks can land here in
    // parallel -- an earlier version reused a member buffer here and would have
    // corrupted it.
    size_t Drain(size_t aBudget = 0)
    {
        std::vector<Task> running;

        {
            std::scoped_lock lock(m_mutex);

            // Nothing queued is the common case, and it allocates nothing.
            if (m_pending.empty())
            {
                return 0;
            }

            if (aBudget == 0 || aBudget >= m_pending.size())
            {
                running.swap(m_pending);
            }
            else
            {
                // A flood must not be allowed to stall a frame, so take a slice and
                // leave the rest for later.
                running.assign(std::make_move_iterator(m_pending.begin()),
                               std::make_move_iterator(m_pending.begin() + aBudget));
                m_pending.erase(m_pending.begin(), m_pending.begin() + aBudget);
            }
        }

        for (auto& task : running)
        {
            task();
        }

        return running.size();
    }

    size_t Pending() const
    {
        std::scoped_lock lock(m_mutex);
        return m_pending.size();
    }

    void Clear()
    {
        std::scoped_lock lock(m_mutex);
        m_pending.clear();
    }

private:
    mutable std::mutex m_mutex;
    std::vector<Task> m_pending;
};
} // namespace core
