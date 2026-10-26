#include "TelemetrySubscriptions.h"
#include <stdexcept>

TelemetrySubscriptions::Subscription::Subscription(Subscription&& other) noexcept
    : slot_(std::move(other.slot_)) {}
TelemetrySubscriptions::Subscription&
TelemetrySubscriptions::Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        Reset();
        slot_ = std::move(other.slot_);
    }
    return *this;
}
TelemetrySubscriptions::Subscription::~Subscription() {
    Reset();
}
void TelemetrySubscriptions::Subscription::Reset() {
    auto slot = std::move(slot_);
    if (!slot)
        return;
    std::lock_guard<std::recursive_mutex> lock(slot->mutex);
    slot->callback = nullptr;
}
TelemetrySubscriptions::TelemetrySubscriptions()
    : worker_(&TelemetrySubscriptions::Run, this), dispatch_id_(worker_.get_id()) {}
TelemetrySubscriptions::~TelemetrySubscriptions() {
    Stop();
}
bool TelemetrySubscriptions::IsDispatchThread() const {
    return std::this_thread::get_id() == dispatch_id_;
}
TelemetrySubscriptions::Subscription TelemetrySubscriptions::Subscribe(Callback callback) {
    if (!callback)
        throw std::invalid_argument("empty telemetry callback");
    auto slot = std::make_shared<Slot>();
    slot->callback = std::make_shared<Callback>(std::move(callback));
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_)
        throw std::logic_error("subscriptions have stopped");
    slots_.push_back(slot);
    return Subscription(slot);
}
void TelemetrySubscriptions::Publish(TelemetryEvent event) {
    Delivery delivery;
    delivery.event = std::make_shared<const TelemetryEvent>(std::move(event));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return;
        // Snapshot membership now: later subscribers must not receive past events.
        for (auto it = slots_.begin(); it != slots_.end();) {
            if (auto slot = it->lock()) {
                delivery.slots.push_back(std::move(slot));
                ++it;
            } else
                it = slots_.erase(it);
        }
        if (delivery.slots.empty())
            return;
        if (queue_.size() >= queue_capacity)
            throw std::overflow_error("telemetry subscriber queue is full");
        queue_.push_back(std::move(delivery));
    }
    ready_.notify_one();
}
void TelemetrySubscriptions::Stop() {
    if (IsDispatchThread())
        throw std::logic_error("Stop must run on the owner thread");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_one();
    if (worker_.joinable())
        worker_.join();
}
void TelemetrySubscriptions::Run() {
    for (;;) {
        Delivery delivery;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty())
                return;
            delivery = std::move(queue_.front());
            queue_.pop_front();
        }
        // Registry lock is released before callbacks or unsubscribe synchronization.
        for (const auto& slot : delivery.slots) {
            std::lock_guard<std::recursive_mutex> lock(slot->mutex);
            if (!slot->callback)
                continue;
            try {
                // Retain the callable through self-reset without copying its mutable state.
                auto callback = slot->callback;
                (*callback)(delivery.event);
            } catch (...) {
                ++callback_errors_;
            }
        }
    }
}
