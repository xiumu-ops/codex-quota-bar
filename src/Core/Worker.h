#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace CodexQuotaBar {
    // One reusable background thread with at most one queued job.
    class Worker {
    public:
        void Submit(std::function<void(std::stop_token)> job) {
            const std::lock_guard submitLock(m_mutex);
            if (!m_thread.joinable()) {
                m_thread = std::jthread([this](std::stop_token stop) {
                    std::unique_lock lock(m_mutex);
                    while (m_ready.wait(lock, stop, [this] { return static_cast<bool>(m_pending); })) {
                        if (stop.stop_requested()) break;
                        auto work = std::exchange(m_pending, nullptr);
                        lock.unlock();
                        work(stop);
                        lock.lock();
                    }
                });
            }
            m_pending = std::move(job);
            m_ready.notify_one();
        }

        void RequestStop() { m_thread.request_stop(); }
        void Join() { if (m_thread.joinable()) m_thread.join(); }

    private:
        std::mutex m_mutex;
        std::condition_variable_any m_ready;
        std::function<void(std::stop_token)> m_pending;
        std::jthread m_thread;
    };
}
