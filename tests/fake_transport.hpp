#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "rp2350_hid_bridge/serial.hpp"

namespace rp2350_hid_bridge::testing {

std::vector<std::uint8_t> response_frame(
    std::uint16_t sequence,
    CommandType type,
    const std::vector<std::uint8_t>& payload = {},
    std::uint8_t flags = 0) {
    return encode_frame(sequence, type, payload, flags);
}

class FakeTransport final : public SerialTransport {
private:
    class HeartbeatThreadExitRecorder {
    public:
        explicit HeartbeatThreadExitRecorder(FakeTransport* transport) : transport_(transport) {}
        ~HeartbeatThreadExitRecorder() { transport_->on_heartbeat_thread_exit(); }

    private:
        FakeTransport* transport_;
    };

public:
    enum class LifecycleEvent {
        StopAllWritten,
        HeartbeatThreadExited,
        CancelledReadExited,
        TransportClosed,
    };

    void open(const std::string& port, std::uint32_t baud, std::uint32_t timeout_ms) override {
        std::lock_guard<std::mutex> lock(mutex_);
        port_ = port;
        baud_ = baud;
        configured_timeout_ms_ = timeout_ms;
        opened_ = true;
        closed_ = false;
        cancelled_ = false;
    }

    void set_dtr(bool asserted) override {
        std::lock_guard<std::mutex> lock(mutex_);
        dtr_history_.push_back(asserted);
    }

    void write(const std::vector<std::uint8_t>& bytes) override {
        const auto frame = decode_frame(bytes);
        if (frame.command_type == CommandType::Heartbeat) {
            thread_local HeartbeatThreadExitRecorder heartbeat_thread_exit(this);
        }
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ++write_attempts_;
            if (fail_next_write_) {
                fail_next_write_ = false;
                throw TransportError("fake write failure");
            }
            ++active_writes_;
            max_active_writes_ = std::max(max_active_writes_, active_writes_);
            writes_.push_back(bytes);
            if (frame.command_type == CommandType::StopAll) {
                lifecycle_events_.push_back(LifecycleEvent::StopAllWritten);
                lifecycle_changed_.notify_all();
            }
            writes_changed_.notify_all();
            if (block_command_ && frame.command_type == *block_command_) {
                blocked_command_entered_ = true;
                block_changed_.notify_all();
                block_changed_.wait(lock, [&] { return release_blocked_command_; });
            }
        }

        if (write_delay_ms_ != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(write_delay_ms_));
        }

        std::optional<std::vector<std::uint8_t>> response;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (frame.command_type != CommandType::Heartbeat) {
                if (!scripted_responses_.empty()) {
                    response = std::move(scripted_responses_.front());
                    scripted_responses_.pop_front();
                } else if (auto_ack_) {
                    auto response_type = CommandType::Ack;
                    std::vector<std::uint8_t> payload;
                    if (batch_aware_) {
                        if (frame.command_type == CommandType::BatchBegin) {
                            if (collecting_) {
                                response_type = CommandType::Nack;
                                payload = {0x0B};
                            } else {
                                collecting_ = true;
                            }
                        } else if (frame.command_type == CommandType::BatchEnd) {
                            if (!collecting_) {
                                response_type = CommandType::Nack;
                                payload = {0x0B};
                            } else {
                                collecting_ = false;
                            }
                        } else if (frame.command_type == CommandType::StopAll) {
                            if (collecting_) {
                                response_type = CommandType::Nack;
                                payload = {0x0B};
                            }
                            collecting_ = false;
                        }
                    }
                    response = response_frame(frame.sequence, response_type, payload);
                }
            }
            if (response) {
                receive_buffer_.insert(receive_buffer_.end(), response->begin(), response->end());
            }
            --active_writes_;
            reads_changed_.notify_all();
        }
    }

    std::vector<std::uint8_t> read(std::size_t max_bytes, std::uint32_t timeout_ms) override {
        std::unique_lock<std::mutex> lock(mutex_);
        read_timeouts_.push_back(timeout_ms);
        ++read_calls_;
        read_entered_ = true;
        reads_changed_.notify_all();
        if (block_reads_) {
            reads_changed_.wait(lock, [&] { return cancelled_ || closed_; });
            if (hold_cancelled_read_ && cancelled_) {
                cancelled_read_held_ = true;
                reads_changed_.notify_all();
                reads_changed_.wait(lock, [&] { return release_cancelled_read_; });
                lifecycle_events_.push_back(LifecycleEvent::CancelledReadExited);
                lifecycle_changed_.notify_all();
            }
        }
        if (cancelled_ || receive_buffer_.empty()) {
            return {};
        }
        const auto count = std::min(max_bytes, receive_buffer_.size());
        std::vector<std::uint8_t> chunk(receive_buffer_.begin(), receive_buffer_.begin() + count);
        receive_buffer_.erase(receive_buffer_.begin(), receive_buffer_.begin() + count);
        return chunk;
    }

    void cancel_read() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
        reads_changed_.notify_all();
    }

    void close() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        opened_ = false;
        lifecycle_events_.push_back(LifecycleEvent::TransportClosed);
        lifecycle_changed_.notify_all();
        reads_changed_.notify_all();
    }

    void gate_heartbeat_thread_exit() {
        std::lock_guard<std::mutex> lock(mutex_);
        gate_heartbeat_thread_exit_ = true;
        release_heartbeat_thread_exit_ = false;
        heartbeat_thread_exit_entered_ = false;
    }

    bool wait_for_heartbeat_thread_exit(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return lifecycle_changed_.wait_for(
            lock,
            timeout,
            [&] { return heartbeat_thread_exit_entered_; });
    }

    void release_heartbeat_thread_exit() {
        std::lock_guard<std::mutex> lock(mutex_);
        release_heartbeat_thread_exit_ = true;
        lifecycle_changed_.notify_all();
    }

    std::vector<LifecycleEvent> lifecycle_events() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lifecycle_events_;
    }

    void set_auto_ack(bool value, bool batch_aware = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto_ack_ = value;
        batch_aware_ = batch_aware;
    }

    void set_scripted_responses(
        std::deque<std::optional<std::vector<std::uint8_t>>> responses) {
        std::lock_guard<std::mutex> lock(mutex_);
        scripted_responses_ = std::move(responses);
    }

    void set_block_reads(bool value) {
        std::lock_guard<std::mutex> lock(mutex_);
        block_reads_ = value;
    }

    void hold_cancelled_read() {
        std::lock_guard<std::mutex> lock(mutex_);
        block_reads_ = true;
        hold_cancelled_read_ = true;
        cancelled_read_held_ = false;
        release_cancelled_read_ = false;
    }

    bool wait_for_cancelled_read(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return reads_changed_.wait_for(lock, timeout, [&] { return cancelled_read_held_; });
    }

    void release_cancelled_read() {
        std::lock_guard<std::mutex> lock(mutex_);
        release_cancelled_read_ = true;
        reads_changed_.notify_all();
    }

    void set_write_delay(std::uint32_t milliseconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        write_delay_ms_ = milliseconds;
    }

    void block_on(CommandType command) {
        std::lock_guard<std::mutex> lock(mutex_);
        block_command_ = command;
        blocked_command_entered_ = false;
        release_blocked_command_ = false;
    }

    bool wait_for_blocked_command(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return block_changed_.wait_for(lock, timeout, [&] { return blocked_command_entered_; });
    }

    void release_blocked_command() {
        std::lock_guard<std::mutex> lock(mutex_);
        release_blocked_command_ = true;
        block_changed_.notify_all();
    }

    bool wait_for_read(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return reads_changed_.wait_for(lock, timeout, [&] { return read_entered_; });
    }

    bool wait_for_command(CommandType command, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return writes_changed_.wait_for(lock, timeout, [&] {
            return std::any_of(writes_.begin(), writes_.end(), [&](const auto& bytes) {
                return decode_frame(bytes).command_type == command;
            });
        });
    }

    std::vector<std::vector<std::uint8_t>> writes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return writes_;
    }

    std::vector<CommandType> command_types() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<CommandType> result;
        for (const auto& bytes : writes_) {
            result.push_back(decode_frame(bytes).command_type);
        }
        return result;
    }

    std::vector<std::uint32_t> read_timeouts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return read_timeouts_;
    }

    std::vector<bool> dtr_history() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dtr_history_;
    }

    int read_calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return read_calls_;
    }

    int write_attempts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return write_attempts_;
    }

    int max_active_writes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return max_active_writes_;
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    void fail_next_write() {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_next_write_ = true;
    }

private:
    void on_heartbeat_thread_exit() {
        std::unique_lock<std::mutex> lock(mutex_);
        heartbeat_thread_exit_entered_ = true;
        lifecycle_changed_.notify_all();
        if (gate_heartbeat_thread_exit_) {
            lifecycle_changed_.wait(lock, [&] { return release_heartbeat_thread_exit_; });
        }
        lifecycle_events_.push_back(LifecycleEvent::HeartbeatThreadExited);
        lifecycle_changed_.notify_all();
    }

    mutable std::mutex mutex_;
    std::condition_variable writes_changed_;
    std::condition_variable reads_changed_;
    std::condition_variable block_changed_;
    std::condition_variable lifecycle_changed_;
    std::string port_;
    std::uint32_t baud_ = 0;
    std::uint32_t configured_timeout_ms_ = 0;
    bool opened_ = false;
    bool closed_ = false;
    bool cancelled_ = false;
    bool auto_ack_ = false;
    bool batch_aware_ = false;
    bool collecting_ = false;
    bool block_reads_ = false;
    bool hold_cancelled_read_ = false;
    bool cancelled_read_held_ = false;
    bool release_cancelled_read_ = false;
    bool read_entered_ = false;
    bool fail_next_write_ = false;
    int read_calls_ = 0;
    int write_attempts_ = 0;
    int active_writes_ = 0;
    int max_active_writes_ = 0;
    std::uint32_t write_delay_ms_ = 0;
    std::vector<bool> dtr_history_;
    std::vector<std::vector<std::uint8_t>> writes_;
    std::vector<std::uint8_t> receive_buffer_;
    std::vector<std::uint32_t> read_timeouts_;
    std::deque<std::optional<std::vector<std::uint8_t>>> scripted_responses_;
    std::optional<CommandType> block_command_;
    bool blocked_command_entered_ = false;
    bool release_blocked_command_ = false;
    bool gate_heartbeat_thread_exit_ = false;
    bool release_heartbeat_thread_exit_ = false;
    bool heartbeat_thread_exit_entered_ = false;
    std::vector<LifecycleEvent> lifecycle_events_;
};

}  // namespace rp2350_hid_bridge::testing
