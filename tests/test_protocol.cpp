#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "rp2350_hid_bridge.hpp"

#define CHECK(expression)                                                                            \
    do {                                                                                             \
        if (!(expression)) {                                                                         \
            throw std::runtime_error(std::string("check failed: ") + #expression);                  \
        }                                                                                            \
    } while (false)

namespace {

using namespace rp2350_hid_bridge;
using namespace std::chrono_literals;

template <typename Function>
void expect_invalid_argument(Function&& function) {
    bool rejected = false;
    try {
        function();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}

template <typename Function>
std::string expect_runtime_error(Function&& function) {
    try {
        function();
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    throw std::runtime_error("expected std::runtime_error");
}

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
                throw std::runtime_error("fake write failure");
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

HidBridgeOptions options(
    int retries = 0,
    std::uint32_t timeout_ms = 100,
    std::uint32_t heartbeat_interval_ms = 60'000) {
    HidBridgeOptions value;
    value.port = "FAKE";
    value.baud = 115200;
    value.timeout_ms = timeout_ms;
    value.retries = retries;
    value.heartbeat_interval_ms = heartbeat_interval_ms;
    return value;
}

void test_protocol_and_keys() {
    auto frame = encode_frame(0x1234, CommandType::Ping, {});
    auto decoded = decode_frame(frame);
    CHECK(decoded.version == 2);
    CHECK(decoded.sequence == 0x1234);
    CHECK(decoded.command_type == CommandType::Ping);
    CHECK(decoded.payload.empty());

    auto heartbeat = encode_frame(0, CommandType::Heartbeat, {}, FLAG_NO_RESPONSE);
    auto decoded_heartbeat = decode_frame(heartbeat);
    CHECK(decoded_heartbeat.version == 2);
    CHECK(decoded_heartbeat.flags == FLAG_NO_RESPONSE);
    CHECK(decoded_heartbeat.sequence == 0);
    CHECK(decoded_heartbeat.command_type == CommandType::Heartbeat);

    auto bad_flags_crc = heartbeat;
    bad_flags_crc[3] = 0;
    bool flags_rejected = false;
    try {
        (void)decode_frame(bad_flags_crc);
    } catch (const DecodeError&) {
        flags_rejected = true;
    }
    CHECK(flags_rejected);

    auto legacy = encode_frame(7, CommandType::Ping, {}, 0, LEGACY_PROTOCOL_VERSION);
    CHECK(decode_frame(legacy).version == LEGACY_PROTOCOL_VERSION);

    auto bad_crc = frame;
    bad_crc.back() ^= 0x55;
    bool rejected = false;
    try {
        (void)decode_frame(bad_crc);
    } catch (const DecodeError&) {
        rejected = true;
    }
    CHECK(rejected);

    auto ctrl_c = parse_combo("CTRL+C");
    CHECK(ctrl_c.modifier == 0x01 && ctrl_c.keycode == 0x06);
    auto shift = parse_combo("SHIFT");
    CHECK(shift.modifier == 0x02 && shift.keycode == 0x00);
    auto ctrl_shift = parse_combo("CTRL+SHIFT");
    CHECK(ctrl_shift.modifier == 0x03 && ctrl_shift.keycode == 0x00);
    expect_invalid_argument([] { (void)parse_combo("CTRL+A+B"); });
    CHECK(parse_combo("ENTER").keycode == 0x28);
    CHECK(parse_combo("F5").keycode == 0x3E);
    CHECK(parse_combo("[").keycode == 0x2F);
    CHECK(parse_combo("BACKSLASH").keycode == 0x31);
    CHECK(parse_combo("CAPSLOCK").keycode == 0x39);
    CHECK(parse_combo("PRINTSCREEN").keycode == 0x46);
}

void test_script_parser() {
    auto commands = parse_script(
        "type \"abc\"\n"
        "key tap ENTER\n"
        "key down CTRL+SHIFT\n"
        "mouse move 10 -5\n"
        "wait 100\n"
        "stop\n");
    CHECK(commands.size() == 6);
    CHECK(commands[0].kind == ScriptKind::Type && commands[0].text == "abc");
    CHECK(commands[1].kind == ScriptKind::Key && commands[1].key_action == KeyAction::Tap);
    CHECK(commands[2].combo.modifier == 0x03 && commands[2].combo.keycode == 0x00);
    CHECK(commands[3].dx == 10 && commands[3].dy == -5);
    CHECK(commands[4].kind == ScriptKind::Wait && commands[4].ms == 100);
    CHECK(commands[5].kind == ScriptKind::Stop);

    CHECK(script_command_to_packet(commands[0]).payload ==
          std::vector<std::uint8_t>({'a', 'b', 'c'}));
    CHECK(script_command_to_packet(commands[1]).payload ==
          std::vector<std::uint8_t>({0x00, 0x28}));
    CHECK(script_command_to_packet(commands[3]).payload ==
          std::vector<std::uint8_t>({0x00, 0x0A, 0xFF, 0xFB}));
    CHECK(script_command_to_packet(commands[4]).payload ==
          std::vector<std::uint8_t>({0x00, 0x00, 0x00, 0x64}));

    expect_invalid_argument([] { (void)parse_script("key press ENTER\n"); });
    expect_invalid_argument([] { (void)parse_script("mouse move 40000 0\n"); });
    expect_invalid_argument([] { (void)parse_script("mouse move 0 -40000\n"); });
    expect_invalid_argument([] { (void)parse_script("mouse wheel 200\n"); });
    expect_invalid_argument([] { (void)parse_script("wait -1\n"); });
    expect_invalid_argument([] { (void)parse_script("wait 100ms\n"); });
}

void test_retry_policy_and_response_parser() {
    {
        auto transport = std::make_shared<FakeTransport>();
        transport->set_scripted_responses({response_frame(1, CommandType::Nack, {0x0F})});
        HidBridge bridge(options(3), transport);
        bridge.open();
        const auto message = expect_runtime_error([&] { bridge.key_down("W"); });
        CHECK(message.find("NACK") != std::string::npos);
        CHECK(transport->writes().size() == 1);
    }

    {
        auto transport = std::make_shared<FakeTransport>();
        transport->set_scripted_responses(
            {std::nullopt, response_frame(1, CommandType::Ack)});
        HidBridge bridge(options(1), transport);
        bridge.open();
        bridge.key_down("W");
        const auto writes = transport->writes();
        CHECK(writes.size() == 2);
        CHECK(writes[0] == writes[1]);
        CHECK(decode_frame(writes[0]).sequence == 1);
    }

    {
        auto transport = std::make_shared<FakeTransport>();
        transport->set_scripted_responses(
            {response_frame(1, CommandType::Busy, {0x03, 0x00, 0xC8}),
             response_frame(1, CommandType::Ack)});
        HidBridge bridge(options(1), transport);
        bridge.open();
        const auto started = std::chrono::steady_clock::now();
        bridge.key_down("W");
        const auto elapsed = std::chrono::steady_clock::now() - started;
        const auto writes = transport->writes();
        CHECK(elapsed >= 150ms);
        CHECK(writes.size() == 2 && writes[0] == writes[1]);
    }

    {
        auto transport = std::make_shared<FakeTransport>();
        auto corrupt = response_frame(99, CommandType::Ack);
        corrupt[7] = 0;
        corrupt[8] = 1;
        auto valid = response_frame(1, CommandType::Ack, {}, 0x40);
        corrupt.insert(corrupt.end(), valid.begin(), valid.end());
        transport->set_scripted_responses({corrupt});
        HidBridge bridge(options(), transport);
        bridge.open();
        const auto response = bridge.send_command(CommandType::Ping);
        CHECK(response.sequence == 1);
        CHECK(response.version == PROTOCOL_VERSION);
        CHECK(response.flags == 0x40);
        CHECK(transport->writes().size() == 1);
    }
}

void test_duration_aware_deadlines() {
    auto transport = std::make_shared<FakeTransport>();
    transport->set_auto_ack(true, true);
    HidBridge bridge(options(), transport);
    bridge.open();

    bridge.ping();
    CHECK(transport->read_timeouts().back() == 1000);
    bridge.wait_ms(2500);
    CHECK(transport->read_timeouts().back() == 3000);
    bridge.type_text("abcdefghij");
    CHECK(transport->read_timeouts().back() == 580);
    bridge.mouse_move(300, -300);
    CHECK(transport->read_timeouts().back() == 503);

    bridge.run_script("type \"abcdefghij\"\nmouse move 300 -300\nwait 2500\n");
    const auto command_types = transport->command_types();
    const auto timeouts = transport->read_timeouts();
    const auto batch_end = std::find(command_types.begin(), command_types.end(), CommandType::BatchEnd);
    CHECK(batch_end != command_types.end());
    const auto batch_end_index = static_cast<std::size_t>(batch_end - command_types.begin());
    CHECK(timeouts[batch_end_index] == 3583);
}

void test_script_stop_segmentation_and_transaction_lock() {
    {
        auto transport = std::make_shared<FakeTransport>();
        transport->set_auto_ack(true, true);
        HidBridge bridge(options(), transport);
        bridge.open();
        bridge.run_script("type \"abc\"\nstop\nwait 25\n");
        CHECK(transport->command_types() == std::vector<CommandType>({
            CommandType::BatchBegin,
            CommandType::TypeAscii,
            CommandType::BatchEnd,
            CommandType::StopAll,
            CommandType::BatchBegin,
            CommandType::WaitMs,
            CommandType::BatchEnd,
        }));
    }

    {
        auto transport = std::make_shared<FakeTransport>();
        transport->set_auto_ack(true, true);
        transport->block_on(CommandType::TypeAscii);
        HidBridge bridge(options(), transport);
        bridge.open();
        std::vector<std::string> errors;
        std::mutex errors_mutex;
        const auto record_error = [&](const std::exception& error) {
            std::lock_guard<std::mutex> lock(errors_mutex);
            errors.push_back(error.what());
        };
        std::thread script_thread([&] {
            try {
                bridge.run_script("type \"abc\"\n");
            } catch (const std::exception& error) {
                record_error(error);
            }
        });
        CHECK(transport->wait_for_blocked_command(1s));
        std::thread ping_thread([&] {
            try {
                bridge.ping();
            } catch (const std::exception& error) {
                record_error(error);
            }
        });
        std::this_thread::sleep_for(20ms);
        transport->release_blocked_command();
        script_thread.join();
        ping_thread.join();
        CHECK(errors.empty());
        CHECK(transport->command_types() == std::vector<CommandType>({
            CommandType::BatchBegin,
            CommandType::TypeAscii,
            CommandType::BatchEnd,
            CommandType::Ping,
        }));
    }
}

void test_open_heartbeat_and_orderly_close() {
    auto transport = std::make_shared<FakeTransport>();
    transport->set_auto_ack(true);
    HidBridge bridge(options(0, 100, 10), transport);
    bridge.open();
    CHECK(transport->wait_for_command(CommandType::Heartbeat, 1s));
    CHECK(transport->read_calls() == 0);

    auto heartbeat_seen = false;
    for (const auto& bytes : transport->writes()) {
        const auto frame = decode_frame(bytes);
        if (frame.command_type == CommandType::Heartbeat) {
            heartbeat_seen = true;
            CHECK(frame.sequence == 0);
            CHECK(frame.flags == FLAG_NO_RESPONSE);
        }
    }
    CHECK(heartbeat_seen);
    bridge.close();
    CHECK(transport->dtr_history() == std::vector<bool>({true, false}));
    CHECK(transport->closed());
    const auto closed_commands = transport->command_types();
    CHECK(!closed_commands.empty() && closed_commands.back() == CommandType::StopAll);
    const auto writes_after_close = transport->writes().size();
    std::this_thread::sleep_for(30ms);
    CHECK(transport->writes().size() == writes_after_close);
    expect_runtime_error([&] { bridge.ping(); });
    CHECK(transport->writes().size() == writes_after_close);
}

void test_close_joins_heartbeat_before_transport_close() {
    auto transport = std::make_shared<FakeTransport>();
    HidBridge bridge(options(0, 100, 1), transport);
    bridge.open();
    CHECK(transport->wait_for_command(CommandType::Heartbeat, 1s));
    transport->gate_heartbeat_thread_exit();

    std::thread closing([&] { bridge.close(); });
    const auto stop_all_seen = transport->wait_for_command(CommandType::StopAll, 1s);
    const auto heartbeat_exit_entered = transport->wait_for_heartbeat_thread_exit(1s);
    const auto closed_before_heartbeat_exit = transport->closed();
    transport->release_heartbeat_thread_exit();
    closing.join();

    CHECK(stop_all_seen);
    CHECK(heartbeat_exit_entered);
    CHECK(!closed_before_heartbeat_exit);
    CHECK(transport->lifecycle_events() == std::vector<FakeTransport::LifecycleEvent>({
        FakeTransport::LifecycleEvent::StopAllWritten,
        FakeTransport::LifecycleEvent::HeartbeatThreadExited,
        FakeTransport::LifecycleEvent::TransportClosed,
    }));
}

void test_command_and_heartbeat_writes_are_serialized() {
    auto transport = std::make_shared<FakeTransport>();
    transport->set_auto_ack(true);
    transport->set_write_delay(15);
    HidBridge bridge(options(0, 100, 1), transport);
    bridge.open();
    CHECK(transport->wait_for_command(CommandType::Heartbeat, 1s));
    bridge.ping();
    bridge.close();
    CHECK(transport->max_active_writes() == 1);
}

void test_close_preempts_active_command_and_stale_waiter() {
    auto transport = std::make_shared<FakeTransport>();
    transport->set_block_reads(true);
    HidBridge bridge(options(2, 100), transport);
    bridge.open();

    std::atomic<int> errors{0};
    std::thread active([&] {
        try {
            bridge.wait_ms(60'000);
        } catch (...) {
            ++errors;
        }
    });
    CHECK(transport->wait_for_read(1s));
    std::thread stale([&] {
        try {
            bridge.ping();
        } catch (...) {
            ++errors;
        }
    });
    std::this_thread::sleep_for(20ms);

    const auto started = std::chrono::steady_clock::now();
    bridge.close();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    bridge.open();
    active.join();
    stale.join();

    CHECK(elapsed < 200ms);
    CHECK(errors == 2);
    const auto commands = transport->command_types();
    CHECK(std::count(commands.begin(), commands.end(), CommandType::WaitMs) == 1);
    CHECK(std::count(commands.begin(), commands.end(), CommandType::Ping) == 0);
    const auto first_stop = std::find(commands.begin(), commands.end(), CommandType::StopAll);
    CHECK(first_stop != commands.end());
    CHECK(std::find(first_stop + 1, commands.end(), CommandType::WaitMs) == commands.end());
    CHECK(std::find(first_stop + 1, commands.end(), CommandType::Ping) == commands.end());
    bridge.close();
}

void test_close_waits_for_cancelled_read_before_transport_close_and_reopen() {
    auto transport = std::make_shared<FakeTransport>();
    HidBridge bridge(options(0, 100), transport);
    bridge.open();
    transport->hold_cancelled_read();

    std::atomic<bool> command_failed{false};
    std::thread command([&] {
        try {
            bridge.wait_ms(60'000);
        } catch (...) {
            command_failed = true;
        }
    });
    CHECK(transport->wait_for_read(1s));

    std::mutex close_mutex;
    std::condition_variable close_changed;
    bool close_finished = false;
    std::thread closing([&] {
        bridge.close();
        {
            std::lock_guard<std::mutex> lock(close_mutex);
            close_finished = true;
        }
        close_changed.notify_all();
    });

    const auto cancelled_read_held = transport->wait_for_cancelled_read(1s);
    const auto stop_all_seen = transport->wait_for_command(CommandType::StopAll, 1s);
    bool close_finished_before_release = false;
    {
        std::unique_lock<std::mutex> lock(close_mutex);
        close_finished_before_release = close_changed.wait_for(lock, 500ms, [&] {
            return close_finished;
        });
    }
    const auto closed_before_release = transport->closed();
    const auto dtr_before_release = transport->dtr_history();
    auto reopen_succeeded_before_release = false;
    try {
        bridge.open();
        reopen_succeeded_before_release = true;
    } catch (const std::runtime_error&) {
    }

    transport->release_cancelled_read();
    command.join();
    closing.join();
    const auto shutdown_events = transport->lifecycle_events();

    CHECK(cancelled_read_held);
    CHECK(stop_all_seen);
    CHECK(!close_finished_before_release);
    CHECK(!closed_before_release);
    CHECK(dtr_before_release == std::vector<bool>({true}));
    CHECK(!reopen_succeeded_before_release);
    CHECK(command_failed);
    CHECK(shutdown_events == std::vector<FakeTransport::LifecycleEvent>({
        FakeTransport::LifecycleEvent::StopAllWritten,
        FakeTransport::LifecycleEvent::CancelledReadExited,
        FakeTransport::LifecycleEvent::TransportClosed,
    }));

    transport->set_block_reads(false);
    transport->set_auto_ack(true);
    bridge.open();
    bridge.ping();
    bridge.close();
}

void test_close_interrupts_busy_delay_without_retry() {
    auto transport = std::make_shared<FakeTransport>();
    transport->set_scripted_responses(
        {response_frame(1, CommandType::Busy, {0x01, 0xFF, 0xFF})});
    HidBridge bridge(options(1), transport);
    bridge.open();
    std::atomic<bool> failed{false};
    std::thread command([&] {
        try {
            bridge.key_down("W");
        } catch (...) {
            failed = true;
        }
    });
    CHECK(transport->wait_for_command(CommandType::KeyDown, 1s));
    std::this_thread::sleep_for(20ms);
    bridge.close();
    command.join();
    CHECK(failed);
    CHECK(transport->command_types() ==
          std::vector<CommandType>({CommandType::KeyDown, CommandType::StopAll}));
}

void test_close_is_best_effort_when_stop_write_fails() {
    auto transport = std::make_shared<FakeTransport>();
    HidBridge bridge(options(), transport);
    bridge.open();
    transport->fail_next_write();
    bridge.close();
    CHECK(transport->write_attempts() == 1);
    CHECK(transport->dtr_history() == std::vector<bool>({true, false}));
    CHECK(transport->closed());
}

}  // namespace

int main() {
    try {
        test_protocol_and_keys();
        test_script_parser();
        test_retry_policy_and_response_parser();
        test_duration_aware_deadlines();
        test_script_stop_segmentation_and_transaction_lock();
        test_open_heartbeat_and_orderly_close();
        test_close_joins_heartbeat_before_transport_close();
        test_command_and_heartbeat_writes_are_serialized();
        test_close_preempts_active_command_and_stale_waiter();
        test_close_waits_for_cancelled_read_before_transport_close_and_reopen();
        test_close_interrupts_busy_delay_without_retry();
        test_close_is_best_effort_when_stop_write_fails();
        std::cout << "C++ SDK protocol v2 tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
