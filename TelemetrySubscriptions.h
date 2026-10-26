#pragma once
#include "TelemetryEvents.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Ordered delivery on one dispatcher thread, never on the Asio thread.
class TelemetrySubscriptions {
    struct Slot;

public:
    using Event = std::shared_ptr<const TelemetryEvent>;
    using Callback = std::function<void(Event)>;
    // Move-only RAII token; reset before destroying referenced callback dependencies.
    class Subscription {
    public:
        Subscription() = default;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        ~Subscription();
        // Waits for an active callback, unless called by that callback itself.
        // Do not hold locks needed by the callback; serialize access to this handle.
        void Reset();

    private:
        friend class TelemetrySubscriptions;
        explicit Subscription(std::shared_ptr<Slot> slot) : slot_(std::move(slot)) {}
        std::shared_ptr<Slot> slot_;
    };
    TelemetrySubscriptions();
    ~TelemetrySubscriptions();
    // Thread-safe registration; callbacks are serialized and may retain the event.
    Subscription Subscribe(Callback callback);
    // Enqueues for current subscribers; throws overflow_error if the queue is full.
    void Publish(TelemetryEvent event);
    // Owner thread only; drains accepted events. Callbacks must eventually return.
    void Stop();
    bool IsDispatchThread() const;
    // Subscriber exceptions are isolated and counted instead of stopping delivery.
    std::uint64_t CallbackErrors() const {
        return callback_errors_.load();
    }

private:
    struct Slot {
        std::recursive_mutex mutex; // Allows Reset() from inside this callback.
        std::shared_ptr<Callback> callback;
    };
    struct Delivery {
        Event event;
        std::vector<std::shared_ptr<Slot>> slots;
    };
    void Run();
    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<std::weak_ptr<Slot>> slots_;
    // Overflow fails publication rather than blocking reception or dropping queued events.
    static constexpr std::size_t queue_capacity = 1024;
    std::deque<Delivery> queue_;
    bool stopping_ = false;
    std::atomic<std::uint64_t> callback_errors_{0};
    std::thread worker_;
    const std::thread::id dispatch_id_;
};
