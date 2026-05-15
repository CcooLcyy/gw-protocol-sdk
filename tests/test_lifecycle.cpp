#include "gw_iec101.h"
#include "gw_iec104.h"
#include "gw_m101.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::chrono::milliseconds kStateWaitTimeout{2000};

std::string status_name(iec_status_t status)
{
    switch (status) {
    case IEC_STATUS_OK:
        return "IEC_STATUS_OK";
    case IEC_STATUS_INVALID_ARGUMENT:
        return "IEC_STATUS_INVALID_ARGUMENT";
    case IEC_STATUS_UNSUPPORTED:
        return "IEC_STATUS_UNSUPPORTED";
    case IEC_STATUS_BAD_STATE:
        return "IEC_STATUS_BAD_STATE";
    case IEC_STATUS_TIMEOUT:
        return "IEC_STATUS_TIMEOUT";
    case IEC_STATUS_NO_MEMORY:
        return "IEC_STATUS_NO_MEMORY";
    case IEC_STATUS_IO_ERROR:
        return "IEC_STATUS_IO_ERROR";
    case IEC_STATUS_PROTOCOL_ERROR:
        return "IEC_STATUS_PROTOCOL_ERROR";
    case IEC_STATUS_BUSY:
        return "IEC_STATUS_BUSY";
    case IEC_STATUS_INTERNAL_ERROR:
        return "IEC_STATUS_INTERNAL_ERROR";
    default:
        return "iec_status_t(" + std::to_string(static_cast<int>(status)) + ")";
    }
}

std::string state_name(iec_runtime_state_t state)
{
    switch (state) {
    case IEC_RUNTIME_CREATED:
        return "CREATED";
    case IEC_RUNTIME_STARTING:
        return "STARTING";
    case IEC_RUNTIME_RUNNING:
        return "RUNNING";
    case IEC_RUNTIME_STOPPING:
        return "STOPPING";
    case IEC_RUNTIME_STOPPED:
        return "STOPPED";
    case IEC_RUNTIME_FAULTED:
        return "FAULTED";
    default:
        return "iec_runtime_state_t(" + std::to_string(static_cast<int>(state)) + ")";
    }
}

std::string states_to_string(const std::vector<iec_runtime_state_t> &states)
{
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < states.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << state_name(states[i]);
    }
    out << "]";
    return out.str();
}

[[noreturn]] void fail_at(int line, const std::string &message)
{
    std::ostringstream out;
    out << "line " << line << ": " << message;
    throw std::runtime_error(out.str());
}

void expect_true(bool value, const char *expr, int line)
{
    if (!value) {
        fail_at(line, std::string("expected true: ") + expr);
    }
}

void expect_status(iec_status_t actual, iec_status_t expected, const char *expr, int line)
{
    if (actual != expected) {
        fail_at(
            line,
            std::string(expr) + " returned " + status_name(actual) + ", expected " + status_name(expected));
    }
}

void expect_state(iec_runtime_state_t actual, iec_runtime_state_t expected, const char *expr, int line)
{
    if (actual != expected) {
        fail_at(
            line,
            std::string(expr) + " was " + state_name(actual) + ", expected " + state_name(expected));
    }
}

#define EXPECT_TRUE(expr) expect_true((expr), #expr, __LINE__)
#define EXPECT_STATUS(expr, expected) expect_status((expr), (expected), #expr, __LINE__)
#define EXPECT_STATE(expr, expected) expect_state((expr), (expected), #expr, __LINE__)

struct MockTransport {
    std::mutex mutex;
    std::condition_variable cv;
    bool closed = false;
    uint32_t send_count = 0;
    uint32_t recv_count = 0;
    std::vector<uint8_t> last_sent;
    std::vector<std::vector<uint8_t>> recv_queue;
};

int mock_send(void *ctx, const uint8_t *data, uint32_t len, uint32_t timeout_ms)
{
    (void)timeout_ms;

    auto *transport = static_cast<MockTransport *>(ctx);
    if (transport == nullptr) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(transport->mutex);
    if (transport->closed) {
        return -1;
    }
    ++transport->send_count;
    transport->last_sent.assign(data, data + len);
    return 0;
}

int mock_recv(void *ctx, uint8_t *buffer, uint32_t capacity, uint32_t *out_len, uint32_t timeout_ms)
{
    auto *transport = static_cast<MockTransport *>(ctx);
    if (transport == nullptr || out_len == nullptr) {
        return -1;
    }

    const uint32_t sleep_ms = std::min<uint32_t>(timeout_ms == 0 ? 1U : timeout_ms, 5U);
    std::unique_lock<std::mutex> lock(transport->mutex);
    transport->cv.wait_for(lock, std::chrono::milliseconds(sleep_ms), [&] {
        return transport->closed || !transport->recv_queue.empty();
    });
    if (transport->closed && transport->recv_queue.empty()) {
        return -1;
    }

    ++transport->recv_count;
    if (transport->recv_queue.empty()) {
        *out_len = 0;
        return 0;
    }

    auto frame = std::move(transport->recv_queue.front());
    transport->recv_queue.erase(transport->recv_queue.begin());
    if (buffer == nullptr || frame.size() > capacity) {
        return -1;
    }

    std::copy(frame.begin(), frame.end(), buffer);
    *out_len = static_cast<uint32_t>(frame.size());
    return 0;
}

void push_recv(MockTransport &transport, std::vector<uint8_t> frame)
{
    {
        std::lock_guard<std::mutex> lock(transport.mutex);
        transport.recv_queue.push_back(std::move(frame));
    }
    transport.cv.notify_all();
}

void close_transport(MockTransport &transport)
{
    {
        std::lock_guard<std::mutex> lock(transport.mutex);
        transport.closed = true;
    }
    transport.cv.notify_all();
}

struct StateRecorder {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::vector<iec_runtime_state_t> states;

    struct RawAsduEvent {
        iec_raw_asdu_direction_t direction = IEC_RAW_ASDU_RX;
        uint16_t common_address = 0;
        uint8_t type_id = 0;
        uint8_t cause_of_transmission = 0;
        uint32_t payload_size = 0;
        uint64_t monotonic_ns = 0;
        std::vector<uint8_t> payload;
    };

    std::vector<RawAsduEvent> raw_events;

    struct PointEvent {
        iec_point_address_t address{};
        iec_point_value_t value{};
    };

    std::vector<PointEvent> point_events;

    struct CommandEvent {
        iec_command_result_t result{};
    };

    std::vector<CommandEvent> command_events;

    void record(iec_runtime_state_t state)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            states.push_back(state);
        }
        cv.notify_all();
    }

    void record_raw(const iec_raw_asdu_event_t &event)
    {
        RawAsduEvent snapshot{};
        snapshot.direction = event.direction;
        snapshot.common_address = event.common_address;
        snapshot.type_id = event.type_id;
        snapshot.cause_of_transmission = event.cause_of_transmission;
        snapshot.payload_size = event.payload_size;
        snapshot.monotonic_ns = event.monotonic_ns;
        if (event.payload != nullptr && event.payload_size > 0) {
            snapshot.payload.assign(event.payload, event.payload + event.payload_size);
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            raw_events.push_back(snapshot);
        }
        cv.notify_all();
    }

    void record_point(const iec_point_address_t &address, const iec_point_value_t &value)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            point_events.push_back(PointEvent{address, value});
        }
        cv.notify_all();
    }

    void record_command(const iec_command_result_t &result)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            command_events.push_back(CommandEvent{result});
        }
        cv.notify_all();
    }

    std::vector<iec_runtime_state_t> snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return states;
    }

    std::vector<RawAsduEvent> raw_snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return raw_events;
    }

    std::vector<PointEvent> point_snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return point_events;
    }

    std::vector<CommandEvent> command_snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return command_events;
    }

    bool wait_until_contains(iec_runtime_state_t state)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, kStateWaitTimeout, [&] {
            return std::find(states.begin(), states.end(), state) != states.end();
        });
    }

    bool wait_until_point_count(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, kStateWaitTimeout, [&] {
            return point_events.size() >= count;
        });
    }

    bool wait_until_command_count(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, kStateWaitTimeout, [&] {
            return command_events.size() >= count;
        });
    }
};

void on_session_state(iec_session_t *session, iec_runtime_state_t state, void *user_context)
{
    (void)session;
    auto *recorder = static_cast<StateRecorder *>(user_context);
    if (recorder == nullptr) {
        return;
    }
    recorder->record(state);
}

void on_raw_asdu(iec_session_t *session, const iec_raw_asdu_event_t *event, void *user_context)
{
    (void)session;
    auto *recorder = static_cast<StateRecorder *>(user_context);
    if (recorder == nullptr || event == nullptr) {
        return;
    }
    recorder->record_raw(*event);
}

void on_point_indication(
    iec_session_t *session,
    const iec_point_address_t *address,
    const iec_point_value_t *value,
    void *user_context)
{
    (void)session;
    auto *recorder = static_cast<StateRecorder *>(user_context);
    if (recorder == nullptr || address == nullptr || value == nullptr) {
        return;
    }
    recorder->record_point(*address, *value);
}

void on_command_result(iec_session_t *session, const iec_command_result_t *result, void *user_context)
{
    (void)session;
    auto *recorder = static_cast<StateRecorder *>(user_context);
    if (recorder == nullptr || result == nullptr) {
        return;
    }
    recorder->record_command(*result);
}

iec_session_config_t make_common_config(StateRecorder &recorder)
{
    iec_session_config_t config{};
    config.user_context = &recorder;
    config.startup_timeout_ms = 1000;
    config.stop_timeout_ms = 1000;
    config.reconnect_interval_ms = 100;
    config.command_timeout_ms = 1000;
    config.enable_raw_asdu = 0;
    config.enable_log_callback = 0;
    config.initial_log_level = 0;
    return config;
}

iec_transport_t make_transport(MockTransport &mock)
{
    iec_transport_t transport{};
    transport.send = mock_send;
    transport.recv = mock_recv;
    transport.ctx = &mock;
    transport.max_plain_frame_len = 253;
    return transport;
}

iec_callbacks_t make_callbacks()
{
    iec_callbacks_t callbacks{};
    callbacks.on_session_state = on_session_state;
    callbacks.on_point_indication = on_point_indication;
    callbacks.on_command_result = on_command_result;
    callbacks.on_raw_asdu = on_raw_asdu;
    return callbacks;
}

iec101_master_config_t make_iec101_config()
{
    iec101_master_config_t config{};
    config.link_mode = IEC101_LINK_MODE_UNBALANCED;
    config.link_address = 1;
    config.link_address_length = 1;
    config.common_address_length = 2;
    config.information_object_address_length = 3;
    config.cot_length = 2;
    config.use_single_char_ack = 1;
    config.ack_timeout_ms = 1000;
    config.repeat_timeout_ms = 1000;
    config.repeat_count = 3;
    return config;
}

m101_master_config_t make_m101_config()
{
    m101_master_config_t config{};
    config.link_mode = IEC101_LINK_MODE_UNBALANCED;
    config.link_address = 1;
    config.link_address_length = 1;
    config.common_address_length = 2;
    config.information_object_address_length = 3;
    config.cot_length = 2;
    config.use_single_char_ack = 1;
    config.ack_timeout_ms = 1000;
    config.repeat_timeout_ms = 1000;
    config.repeat_count = 3;
    config.preferred_file_chunk_size = 256;
    return config;
}

iec104_master_config_t make_iec104_config()
{
    iec104_master_config_t config{};
    config.common_address_length = 2;
    config.information_object_address_length = 3;
    config.cot_length = 2;
    config.k = 12;
    config.w = 8;
    config.t0_ms = 30000;
    config.t1_ms = 15000;
    config.t2_ms = 10000;
    config.t3_ms = 20000;
    return config;
}

void expect_state_history(
    const StateRecorder &recorder,
    const std::vector<iec_runtime_state_t> &expected,
    int line)
{
    const auto actual = recorder.snapshot();
    if (actual != expected) {
        fail_at(line, "state callbacks were " + states_to_string(actual) + ", expected " + states_to_string(expected));
    }
}

#define EXPECT_STATE_HISTORY(recorder, expected) expect_state_history((recorder), (expected), __LINE__)

template <typename Config, typename ValidateFn>
void exercise_validate_config(
    const char *name,
    const Config &valid_config,
    ValidateFn validate,
    Config invalid_config)
{
    EXPECT_STATUS(validate(&valid_config), IEC_STATUS_OK);
    EXPECT_STATUS(validate(nullptr), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_STATUS(validate(&invalid_config), IEC_STATUS_INVALID_ARGUMENT);
    std::printf("  %s validate_config\n", name);
}

template <
    typename Config,
    typename CreateFn,
    typename DestroyFn,
    typename GetStateFn,
    typename StartFn,
    typename StopFn,
    typename SetOptionFn>
void exercise_lifecycle(
    const char *name,
    const Config &protocol_config,
    CreateFn create,
    DestroyFn destroy,
    GetStateFn get_state,
    StartFn start,
    StopFn stop,
    SetOptionFn set_option)
{
    StateRecorder recorder;
    MockTransport mock;
    auto common = make_common_config(recorder);
    auto transport = make_transport(mock);
    auto callbacks = make_callbacks();

    iec_session_t *session = nullptr;

    EXPECT_STATUS(create(nullptr, &protocol_config, &transport, &callbacks, &session), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_STATUS(create(&common, nullptr, &transport, &callbacks, &session), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_STATUS(create(&common, &protocol_config, nullptr, &callbacks, &session), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_STATUS(create(&common, &protocol_config, &transport, nullptr, &session), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_STATUS(create(&common, &protocol_config, &transport, &callbacks, nullptr), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_STATUS(start(nullptr), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_STATUS(stop(nullptr, 100), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_STATUS(destroy(nullptr), IEC_STATUS_INVALID_ARGUMENT);

    iec_runtime_state_t state = IEC_RUNTIME_FAULTED;
    EXPECT_STATUS(get_state(nullptr, &state), IEC_STATUS_INVALID_ARGUMENT);

    uint8_t raw_asdu_enabled = 1;
    EXPECT_STATUS(
        set_option(nullptr, IEC_OPTION_ENABLE_RAW_ASDU, &raw_asdu_enabled, sizeof(raw_asdu_enabled)),
        IEC_STATUS_INVALID_ARGUMENT);

    EXPECT_STATUS(create(&common, &protocol_config, &transport, &callbacks, &session), IEC_STATUS_OK);
    EXPECT_TRUE(session != nullptr);

    auto cleanup = [&] {
        if (session != nullptr) {
            (void)stop(session, 100);
            (void)destroy(session);
            session = nullptr;
        }
        close_transport(mock);
    };

    try {
        EXPECT_STATUS(get_state(session, nullptr), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(get_state(session, &state), IEC_STATUS_OK);
        EXPECT_STATE(state, IEC_RUNTIME_CREATED);
        EXPECT_STATUS(stop(session, 100), IEC_STATUS_BAD_STATE);

        EXPECT_STATUS(
            set_option(session, IEC_OPTION_ENABLE_RAW_ASDU, nullptr, sizeof(raw_asdu_enabled)),
            IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(
            set_option(session, IEC_OPTION_ENABLE_RAW_ASDU, &raw_asdu_enabled, 0),
            IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(
            set_option(session, static_cast<iec_option_t>(0), &raw_asdu_enabled, sizeof(raw_asdu_enabled)),
            IEC_STATUS_INVALID_ARGUMENT);
        uint8_t log_level = IEC_LOG_INFO;
        EXPECT_STATUS(
            set_option(session, IEC_OPTION_LOG_LEVEL, &log_level, sizeof(log_level)),
            IEC_STATUS_OK);

        EXPECT_STATUS(start(session), IEC_STATUS_OK);
        EXPECT_TRUE(recorder.wait_until_contains(IEC_RUNTIME_RUNNING));
        EXPECT_STATUS(get_state(session, &state), IEC_STATUS_OK);
        EXPECT_STATE(state, IEC_RUNTIME_RUNNING);

        EXPECT_STATUS(start(session), IEC_STATUS_BAD_STATE);
        const iec_status_t destroy_running = destroy(session);
        if (destroy_running == IEC_STATUS_OK) {
            session = nullptr;
            close_transport(mock);
        }
        EXPECT_STATUS(destroy_running, IEC_STATUS_BAD_STATE);

        EXPECT_STATUS(stop(session, 1000), IEC_STATUS_OK);
        EXPECT_TRUE(recorder.wait_until_contains(IEC_RUNTIME_STOPPED));
        EXPECT_STATUS(get_state(session, &state), IEC_STATUS_OK);
        EXPECT_STATE(state, IEC_RUNTIME_STOPPED);
        expect_state_history(
            recorder,
            std::vector<iec_runtime_state_t>{
                IEC_RUNTIME_STARTING,
                IEC_RUNTIME_RUNNING,
                IEC_RUNTIME_STOPPING,
                IEC_RUNTIME_STOPPED,
            },
            __LINE__);

        EXPECT_STATUS(destroy(session), IEC_STATUS_OK);
        session = nullptr;
        close_transport(mock);
    } catch (...) {
        cleanup();
        throw;
    }

    std::printf("  %s lifecycle\n", name);
}

template <
    typename Config,
    typename CreateFn,
    typename DestroyFn,
    typename StartFn,
    typename StopFn,
    typename SetOptionFn,
    typename SendRawFn>
void exercise_raw_asdu(
    const char *name,
    const Config &protocol_config,
    CreateFn create,
    DestroyFn destroy,
    StartFn start,
    StopFn stop,
    SetOptionFn set_option,
    SendRawFn send_raw)
{
    StateRecorder recorder;
    MockTransport mock;
    auto common = make_common_config(recorder);
    auto transport = make_transport(mock);
    auto callbacks = make_callbacks();

    iec_session_t *session = nullptr;
    const uint8_t raw_payload[] = {100, 1, 6, 0, 1, 0};
    const auto raw_payload_size = static_cast<uint32_t>(sizeof(raw_payload));
    const std::vector<uint8_t> expected_raw(raw_payload, raw_payload + raw_payload_size);
    const iec_raw_asdu_tx_t raw_request{raw_payload, raw_payload_size, 0};
    const uint8_t short_raw_payload[] = {100, 1, 6};
    const iec_raw_asdu_tx_t short_raw_request{
        short_raw_payload,
        static_cast<uint32_t>(sizeof(short_raw_payload)),
        0};
    const iec_raw_asdu_tx_t null_payload_request{nullptr, raw_payload_size, 0};
    const iec_raw_asdu_tx_t invalid_flag_request{raw_payload, raw_payload_size, 2};

    EXPECT_STATUS(send_raw(nullptr, &raw_request), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_STATUS(create(&common, &protocol_config, &transport, &callbacks, &session), IEC_STATUS_OK);
    EXPECT_TRUE(session != nullptr);

    auto cleanup = [&] {
        if (session != nullptr) {
            (void)stop(session, 100);
            (void)destroy(session);
            session = nullptr;
        }
        close_transport(mock);
    };

    try {
        EXPECT_STATUS(send_raw(session, nullptr), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(send_raw(session, &raw_request), IEC_STATUS_BAD_STATE);

        EXPECT_STATUS(start(session), IEC_STATUS_OK);
        EXPECT_TRUE(recorder.wait_until_contains(IEC_RUNTIME_RUNNING));
        EXPECT_STATUS(send_raw(session, &raw_request), IEC_STATUS_BAD_STATE);

        uint8_t raw_asdu_enabled = 1;
        EXPECT_STATUS(
            set_option(session, IEC_OPTION_ENABLE_RAW_ASDU, &raw_asdu_enabled, sizeof(raw_asdu_enabled)),
            IEC_STATUS_OK);
        EXPECT_STATUS(send_raw(session, &null_payload_request), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(send_raw(session, &short_raw_request), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(send_raw(session, &invalid_flag_request), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(send_raw(session, &raw_request), IEC_STATUS_OK);

        EXPECT_TRUE(mock.send_count == 1);
        EXPECT_TRUE(mock.last_sent == expected_raw);
        const auto raw_events = recorder.raw_snapshot();
        EXPECT_TRUE(raw_events.size() == 1);
        EXPECT_TRUE(raw_events[0].direction == IEC_RAW_ASDU_TX);
        EXPECT_TRUE(raw_events[0].common_address == 1);
        EXPECT_TRUE(raw_events[0].type_id == 100);
        EXPECT_TRUE(raw_events[0].cause_of_transmission == 6);
        EXPECT_TRUE(raw_events[0].payload_size == raw_payload_size);
        EXPECT_TRUE(raw_events[0].payload == expected_raw);
        EXPECT_TRUE(raw_events[0].monotonic_ns > 0);

        EXPECT_STATUS(stop(session, 1000), IEC_STATUS_OK);
        EXPECT_STATUS(destroy(session), IEC_STATUS_OK);
        session = nullptr;
        close_transport(mock);
    } catch (...) {
        cleanup();
        throw;
    }

    std::printf("  %s raw_asdu\n", name);
}

template <
    typename Config,
    typename CreateFn,
    typename DestroyFn,
    typename StartFn,
    typename StopFn,
    typename GeneralInterrogationFn>
void exercise_general_interrogation(
    const char *name,
    const Config &protocol_config,
    CreateFn create,
    DestroyFn destroy,
    StartFn start,
    StopFn stop,
    GeneralInterrogationFn general_interrogation)
{
    StateRecorder recorder;
    MockTransport mock;
    auto common = make_common_config(recorder);
    auto transport = make_transport(mock);
    auto callbacks = make_callbacks();

    iec_session_t *session = nullptr;
    const iec_interrogation_request_t request{1, 20};
    const iec_interrogation_request_t invalid_request{1, 0};
    const std::vector<uint8_t> expected_request{
        100,
        1,
        6,
        0,
        1,
        0,
        0,
        0,
        0,
        20,
    };
    const std::vector<uint8_t> point_response{
        1,
        1,
        20,
        0,
        1,
        0,
        0x01,
        0x40,
        0x00,
        0x01,
    };

    EXPECT_STATUS(general_interrogation(nullptr, &request), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_STATUS(create(&common, &protocol_config, &transport, &callbacks, &session), IEC_STATUS_OK);
    EXPECT_TRUE(session != nullptr);

    auto cleanup = [&] {
        if (session != nullptr) {
            (void)stop(session, 100);
            (void)destroy(session);
            session = nullptr;
        }
        close_transport(mock);
    };

    try {
        EXPECT_STATUS(general_interrogation(session, nullptr), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(general_interrogation(session, &invalid_request), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(general_interrogation(session, &request), IEC_STATUS_BAD_STATE);

        EXPECT_STATUS(start(session), IEC_STATUS_OK);
        EXPECT_TRUE(recorder.wait_until_contains(IEC_RUNTIME_RUNNING));
        EXPECT_STATUS(general_interrogation(session, &request), IEC_STATUS_OK);
        EXPECT_TRUE(mock.send_count == 1);
        EXPECT_TRUE(mock.last_sent == expected_request);

        push_recv(mock, point_response);
        EXPECT_TRUE(recorder.wait_until_point_count(1));
        const auto points = recorder.point_snapshot();
        EXPECT_TRUE(points.size() == 1);
        EXPECT_TRUE(points[0].address.common_address == 1);
        EXPECT_TRUE(points[0].address.information_object_address == 0x4001);
        EXPECT_TRUE(points[0].address.type_id == 1);
        EXPECT_TRUE(points[0].address.cause_of_transmission == 20);
        EXPECT_TRUE(points[0].address.originator_address == 0);
        EXPECT_TRUE(points[0].value.point_type == IEC_POINT_SINGLE);
        EXPECT_TRUE(points[0].value.data.single == 1);
        EXPECT_TRUE(points[0].value.quality == 0);
        EXPECT_TRUE(points[0].value.has_timestamp == 0);
        EXPECT_TRUE(points[0].value.is_sequence == 0);

        EXPECT_STATUS(stop(session, 1000), IEC_STATUS_OK);
        EXPECT_STATUS(destroy(session), IEC_STATUS_OK);
        session = nullptr;
        close_transport(mock);
    } catch (...) {
        cleanup();
        throw;
    }

    std::printf("  %s general_interrogation\n", name);
}

template <
    typename Config,
    typename CreateFn,
    typename DestroyFn,
    typename StartFn,
    typename StopFn,
    typename CounterInterrogationFn>
void exercise_counter_interrogation(
    const char *name,
    const Config &protocol_config,
    CreateFn create,
    DestroyFn destroy,
    StartFn start,
    StopFn stop,
    CounterInterrogationFn counter_interrogation)
{
    StateRecorder recorder;
    MockTransport mock;
    auto common = make_common_config(recorder);
    auto transport = make_transport(mock);
    auto callbacks = make_callbacks();

    iec_session_t *session = nullptr;
    const iec_counter_interrogation_request_t request{1, 5, 0};
    const iec_counter_interrogation_request_t invalid_qualifier{1, 0, 0};
    const iec_counter_interrogation_request_t invalid_freeze{1, 5, 4};
    const std::vector<uint8_t> expected_request{
        101,
        1,
        6,
        0,
        1,
        0,
        0,
        0,
        0,
        5,
    };
    const std::vector<uint8_t> counter_response{
        15,
        1,
        37,
        0,
        1,
        0,
        0x01,
        0x40,
        0x00,
        0x78,
        0x56,
        0x34,
        0x12,
        0,
    };

    EXPECT_STATUS(counter_interrogation(nullptr, &request), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_STATUS(create(&common, &protocol_config, &transport, &callbacks, &session), IEC_STATUS_OK);
    EXPECT_TRUE(session != nullptr);

    auto cleanup = [&] {
        if (session != nullptr) {
            (void)stop(session, 100);
            (void)destroy(session);
            session = nullptr;
        }
        close_transport(mock);
    };

    try {
        EXPECT_STATUS(counter_interrogation(session, nullptr), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(counter_interrogation(session, &invalid_qualifier), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(counter_interrogation(session, &invalid_freeze), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(counter_interrogation(session, &request), IEC_STATUS_BAD_STATE);

        EXPECT_STATUS(start(session), IEC_STATUS_OK);
        EXPECT_TRUE(recorder.wait_until_contains(IEC_RUNTIME_RUNNING));
        EXPECT_STATUS(counter_interrogation(session, &request), IEC_STATUS_OK);
        EXPECT_TRUE(mock.send_count == 1);
        EXPECT_TRUE(mock.last_sent == expected_request);

        push_recv(mock, counter_response);
        EXPECT_TRUE(recorder.wait_until_point_count(1));
        const auto points = recorder.point_snapshot();
        EXPECT_TRUE(points.size() == 1);
        EXPECT_TRUE(points[0].address.common_address == 1);
        EXPECT_TRUE(points[0].address.information_object_address == 0x4001);
        EXPECT_TRUE(points[0].address.type_id == 15);
        EXPECT_TRUE(points[0].address.cause_of_transmission == 37);
        EXPECT_TRUE(points[0].address.originator_address == 0);
        EXPECT_TRUE(points[0].value.point_type == IEC_POINT_INTEGRATED_TOTAL);
        EXPECT_TRUE(points[0].value.data.integrated_total == 0x12345678);
        EXPECT_TRUE(points[0].value.quality == 0);

        EXPECT_STATUS(stop(session, 1000), IEC_STATUS_OK);
        EXPECT_STATUS(destroy(session), IEC_STATUS_OK);
        session = nullptr;
        close_transport(mock);
    } catch (...) {
        cleanup();
        throw;
    }

    std::printf("  %s counter_interrogation\n", name);
}

template <
    typename Config,
    typename CreateFn,
    typename DestroyFn,
    typename StartFn,
    typename StopFn,
    typename ControlPointFn>
void exercise_control_point(
    const char *name,
    const Config &protocol_config,
    CreateFn create,
    DestroyFn destroy,
    StartFn start,
    StopFn stop,
    ControlPointFn control_point)
{
    StateRecorder recorder;
    MockTransport mock;
    auto common = make_common_config(recorder);
    auto transport = make_transport(mock);
    auto callbacks = make_callbacks();

    iec_session_t *session = nullptr;
    iec_command_request_t request{};
    request.address.common_address = 1;
    request.address.information_object_address = 0x4001;
    request.address.originator_address = 7;
    request.command_type = IEC_COMMAND_DOUBLE;
    request.semantic = IEC_COMMAND_SEMANTIC_FACTORY_RESET;
    request.mode = IEC_COMMAND_MODE_SELECT;
    request.qualifier = 3;
    request.execute_on_ack = 1;
    request.timeout_ms = 3000;
    request.value.doubled = 2;

    auto invalid_type = request;
    invalid_type.command_type = static_cast<iec_command_type_t>(0);
    auto invalid_semantic = request;
    invalid_semantic.semantic = static_cast<iec_command_semantic_t>(99);
    auto invalid_mode = request;
    invalid_mode.mode = static_cast<iec_command_mode_t>(0);
    auto invalid_flag = request;
    invalid_flag.execute_on_ack = 2;
    auto invalid_address = request;
    invalid_address.address.information_object_address = 0x01000000;
    auto invalid_factory_reset_type = request;
    invalid_factory_reset_type.command_type = IEC_COMMAND_SETPOINT_SCALED;
    invalid_factory_reset_type.value.scaled = 10;
    auto invalid_double_value = request;
    invalid_double_value.value.doubled = 4;

    const std::vector<uint8_t> expected_request{
        46,
        1,
        6,
        7,
        1,
        0,
        0x01,
        0x40,
        0x00,
        0x8e,
    };
    const std::vector<uint8_t> command_response{
        46,
        1,
        7,
        7,
        1,
        0,
        0x01,
        0x40,
        0x00,
        0x02,
    };

    uint32_t command_id = 99;
    EXPECT_STATUS(control_point(nullptr, &request, &command_id), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_TRUE(command_id == 0);
    EXPECT_STATUS(create(&common, &protocol_config, &transport, &callbacks, &session), IEC_STATUS_OK);
    EXPECT_TRUE(session != nullptr);

    auto cleanup = [&] {
        if (session != nullptr) {
            (void)stop(session, 100);
            (void)destroy(session);
            session = nullptr;
        }
        close_transport(mock);
    };

    try {
        command_id = 99;
        EXPECT_STATUS(control_point(session, nullptr, &command_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_TRUE(command_id == 0);
        EXPECT_STATUS(control_point(session, &request, nullptr), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(control_point(session, &invalid_type, &command_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(control_point(session, &invalid_semantic, &command_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(control_point(session, &invalid_mode, &command_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(control_point(session, &invalid_flag, &command_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(control_point(session, &invalid_factory_reset_type, &command_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(control_point(session, &invalid_double_value, &command_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(control_point(session, &request, &command_id), IEC_STATUS_BAD_STATE);
        EXPECT_TRUE(command_id == 0);

        EXPECT_STATUS(start(session), IEC_STATUS_OK);
        EXPECT_TRUE(recorder.wait_until_contains(IEC_RUNTIME_RUNNING));
        EXPECT_STATUS(control_point(session, &invalid_address, &command_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(control_point(session, &request, &command_id), IEC_STATUS_OK);
        EXPECT_TRUE(command_id == 1);
        EXPECT_TRUE(mock.send_count == 1);
        EXPECT_TRUE(mock.last_sent == expected_request);

        uint32_t second_command_id = 0;
        EXPECT_STATUS(control_point(session, &request, &second_command_id), IEC_STATUS_OK);
        EXPECT_TRUE(second_command_id == 2);
        EXPECT_TRUE(mock.send_count == 2);

        push_recv(mock, command_response);
        EXPECT_TRUE(recorder.wait_until_command_count(1));
        const auto commands = recorder.command_snapshot();
        EXPECT_TRUE(commands.size() == 1);
        EXPECT_TRUE(commands[0].result.command_id == 1);
        EXPECT_TRUE(commands[0].result.semantic == IEC_COMMAND_SEMANTIC_FACTORY_RESET);
        EXPECT_TRUE(commands[0].result.result == IEC_COMMAND_RESULT_ACCEPTED);
        EXPECT_TRUE(commands[0].result.is_final == 1);
        EXPECT_TRUE(commands[0].result.address.common_address == 1);
        EXPECT_TRUE(commands[0].result.address.information_object_address == 0x4001);
        EXPECT_TRUE(commands[0].result.address.type_id == 46);
        EXPECT_TRUE(commands[0].result.address.cause_of_transmission == 7);
        EXPECT_TRUE(commands[0].result.address.originator_address == 7);

        EXPECT_STATUS(stop(session, 1000), IEC_STATUS_OK);
        EXPECT_STATUS(destroy(session), IEC_STATUS_OK);
        session = nullptr;
        close_transport(mock);
    } catch (...) {
        cleanup();
        throw;
    }

    std::printf("  %s control_point\n", name);
}

template <
    typename Config,
    typename CreateFn,
    typename DestroyFn,
    typename StartFn,
    typename StopFn,
    typename ReadPointFn>
void exercise_read_point(
    const char *name,
    const Config &protocol_config,
    CreateFn create,
    DestroyFn destroy,
    StartFn start,
    StopFn stop,
    ReadPointFn read_point)
{
    StateRecorder recorder;
    MockTransport mock;
    auto common = make_common_config(recorder);
    auto transport = make_transport(mock);
    auto callbacks = make_callbacks();

    iec_session_t *session = nullptr;
    const iec_point_address_t address{1, 0x4001, 0, 0, 7};
    iec_point_address_t invalid_address = address;
    invalid_address.information_object_address = 0x01000000;
    const std::vector<uint8_t> expected_request{
        102,
        1,
        5,
        7,
        1,
        0,
        0x01,
        0x40,
        0x00,
    };
    const std::vector<uint8_t> point_response{
        1,
        1,
        5,
        7,
        1,
        0,
        0x01,
        0x40,
        0x00,
        0x01,
    };

    EXPECT_STATUS(read_point(nullptr, &address), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_STATUS(create(&common, &protocol_config, &transport, &callbacks, &session), IEC_STATUS_OK);
    EXPECT_TRUE(session != nullptr);

    auto cleanup = [&] {
        if (session != nullptr) {
            (void)stop(session, 100);
            (void)destroy(session);
            session = nullptr;
        }
        close_transport(mock);
    };

    try {
        EXPECT_STATUS(read_point(session, nullptr), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(read_point(session, &address), IEC_STATUS_BAD_STATE);

        EXPECT_STATUS(start(session), IEC_STATUS_OK);
        EXPECT_TRUE(recorder.wait_until_contains(IEC_RUNTIME_RUNNING));
        EXPECT_STATUS(read_point(session, &invalid_address), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(read_point(session, &address), IEC_STATUS_OK);
        EXPECT_TRUE(mock.send_count == 1);
        EXPECT_TRUE(mock.last_sent == expected_request);

        push_recv(mock, point_response);
        EXPECT_TRUE(recorder.wait_until_point_count(1));
        const auto points = recorder.point_snapshot();
        EXPECT_TRUE(points.size() == 1);
        EXPECT_TRUE(points[0].address.common_address == 1);
        EXPECT_TRUE(points[0].address.information_object_address == 0x4001);
        EXPECT_TRUE(points[0].address.type_id == 1);
        EXPECT_TRUE(points[0].address.cause_of_transmission == 5);
        EXPECT_TRUE(points[0].address.originator_address == 7);
        EXPECT_TRUE(points[0].value.point_type == IEC_POINT_SINGLE);
        EXPECT_TRUE(points[0].value.data.single == 1);
        EXPECT_TRUE(points[0].value.quality == 0);

        EXPECT_STATUS(stop(session, 1000), IEC_STATUS_OK);
        EXPECT_STATUS(destroy(session), IEC_STATUS_OK);
        session = nullptr;
        close_transport(mock);
    } catch (...) {
        cleanup();
        throw;
    }

    std::printf("  %s read_point\n", name);
}

void test_iec101_validate_config()
{
    auto valid = make_iec101_config();
    auto invalid = valid;
    invalid.common_address_length = 0;

    exercise_validate_config(
        "iec101",
        valid,
        [](const iec101_master_config_t *config) { return iec101_validate_config(config); },
        invalid);
}

void test_iec101_lifecycle()
{
    const auto valid = make_iec101_config();
    exercise_lifecycle(
        "iec101",
        valid,
        [](const iec_session_config_t *common,
           const iec101_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return iec101_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return iec101_destroy(session); },
        [](const iec_session_t *session, iec_runtime_state_t *out_state) {
            return iec101_get_runtime_state(session, out_state);
        },
        [](iec_session_t *session) { return iec101_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return iec101_stop(session, timeout_ms); },
        [](iec_session_t *session, iec_option_t option, const void *value, uint32_t value_size) {
            return iec101_set_option(session, option, value, value_size);
        });
}

void test_iec101_raw_asdu()
{
    const auto valid = make_iec101_config();
    exercise_raw_asdu(
        "iec101",
        valid,
        [](const iec_session_config_t *common,
           const iec101_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return iec101_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return iec101_destroy(session); },
        [](iec_session_t *session) { return iec101_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return iec101_stop(session, timeout_ms); },
        [](iec_session_t *session, iec_option_t option, const void *value, uint32_t value_size) {
            return iec101_set_option(session, option, value, value_size);
        },
        [](iec_session_t *session, const iec_raw_asdu_tx_t *request) {
            return iec101_send_raw_asdu(session, request);
        });
}

void test_iec101_general_interrogation()
{
    const auto valid = make_iec101_config();
    exercise_general_interrogation(
        "iec101",
        valid,
        [](const iec_session_config_t *common,
           const iec101_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return iec101_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return iec101_destroy(session); },
        [](iec_session_t *session) { return iec101_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return iec101_stop(session, timeout_ms); },
        [](iec_session_t *session, const iec_interrogation_request_t *request) {
            return iec101_general_interrogation(session, request);
        });
}

void test_iec101_counter_interrogation()
{
    const auto valid = make_iec101_config();
    exercise_counter_interrogation(
        "iec101",
        valid,
        [](const iec_session_config_t *common,
           const iec101_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return iec101_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return iec101_destroy(session); },
        [](iec_session_t *session) { return iec101_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return iec101_stop(session, timeout_ms); },
        [](iec_session_t *session, const iec_counter_interrogation_request_t *request) {
            return iec101_counter_interrogation(session, request);
        });
}

void test_iec101_control_point()
{
    const auto valid = make_iec101_config();
    exercise_control_point(
        "iec101",
        valid,
        [](const iec_session_config_t *common,
           const iec101_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return iec101_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return iec101_destroy(session); },
        [](iec_session_t *session) { return iec101_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return iec101_stop(session, timeout_ms); },
        [](iec_session_t *session, const iec_command_request_t *request, uint32_t *out_command_id) {
            return iec101_control_point(session, request, out_command_id);
        });
}

void test_iec101_read_point()
{
    const auto valid = make_iec101_config();
    exercise_read_point(
        "iec101",
        valid,
        [](const iec_session_config_t *common,
           const iec101_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return iec101_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return iec101_destroy(session); },
        [](iec_session_t *session) { return iec101_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return iec101_stop(session, timeout_ms); },
        [](iec_session_t *session, const iec_point_address_t *address) {
            return iec101_read_point(session, address);
        });
}

void test_m101_validate_config()
{
    auto valid = make_m101_config();
    auto invalid = valid;
    invalid.common_address_length = 0;

    exercise_validate_config(
        "m101",
        valid,
        [](const m101_master_config_t *config) { return m101_validate_config(config); },
        invalid);
}

void test_m101_lifecycle()
{
    const auto valid = make_m101_config();
    exercise_lifecycle(
        "m101",
        valid,
        [](const iec_session_config_t *common,
           const m101_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return m101_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return m101_destroy(session); },
        [](const iec_session_t *session, iec_runtime_state_t *out_state) {
            return m101_get_runtime_state(session, out_state);
        },
        [](iec_session_t *session) { return m101_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return m101_stop(session, timeout_ms); },
        [](iec_session_t *session, iec_option_t option, const void *value, uint32_t value_size) {
            return m101_set_option(session, option, value, value_size);
        });
}

void test_m101_raw_asdu()
{
    const auto valid = make_m101_config();
    exercise_raw_asdu(
        "m101",
        valid,
        [](const iec_session_config_t *common,
           const m101_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return m101_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return m101_destroy(session); },
        [](iec_session_t *session) { return m101_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return m101_stop(session, timeout_ms); },
        [](iec_session_t *session, iec_option_t option, const void *value, uint32_t value_size) {
            return m101_set_option(session, option, value, value_size);
        },
        [](iec_session_t *session, const iec_raw_asdu_tx_t *request) {
            return m101_send_raw_asdu(session, request);
        });
}

void test_m101_general_interrogation()
{
    const auto valid = make_m101_config();
    exercise_general_interrogation(
        "m101",
        valid,
        [](const iec_session_config_t *common,
           const m101_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return m101_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return m101_destroy(session); },
        [](iec_session_t *session) { return m101_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return m101_stop(session, timeout_ms); },
        [](iec_session_t *session, const iec_interrogation_request_t *request) {
            return m101_general_interrogation(session, request);
        });
}

void test_m101_counter_interrogation()
{
    const auto valid = make_m101_config();
    exercise_counter_interrogation(
        "m101",
        valid,
        [](const iec_session_config_t *common,
           const m101_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return m101_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return m101_destroy(session); },
        [](iec_session_t *session) { return m101_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return m101_stop(session, timeout_ms); },
        [](iec_session_t *session, const iec_counter_interrogation_request_t *request) {
            return m101_counter_interrogation(session, request);
        });
}

void test_m101_control_point()
{
    const auto valid = make_m101_config();
    exercise_control_point(
        "m101",
        valid,
        [](const iec_session_config_t *common,
           const m101_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return m101_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return m101_destroy(session); },
        [](iec_session_t *session) { return m101_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return m101_stop(session, timeout_ms); },
        [](iec_session_t *session, const iec_command_request_t *request, uint32_t *out_command_id) {
            return m101_control_point(session, request, out_command_id);
        });
}

void test_m101_read_point()
{
    const auto valid = make_m101_config();
    exercise_read_point(
        "m101",
        valid,
        [](const iec_session_config_t *common,
           const m101_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return m101_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return m101_destroy(session); },
        [](iec_session_t *session) { return m101_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return m101_stop(session, timeout_ms); },
        [](iec_session_t *session, const iec_point_address_t *address) {
            return m101_read_point(session, address);
        });
}

void test_iec104_validate_config()
{
    auto valid = make_iec104_config();
    auto invalid = valid;
    invalid.common_address_length = 0;

    exercise_validate_config(
        "iec104",
        valid,
        [](const iec104_master_config_t *config) { return iec104_validate_config(config); },
        invalid);
}

void test_iec104_lifecycle()
{
    const auto valid = make_iec104_config();
    exercise_lifecycle(
        "iec104",
        valid,
        [](const iec_session_config_t *common,
           const iec104_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return iec104_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return iec104_destroy(session); },
        [](const iec_session_t *session, iec_runtime_state_t *out_state) {
            return iec104_get_runtime_state(session, out_state);
        },
        [](iec_session_t *session) { return iec104_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return iec104_stop(session, timeout_ms); },
        [](iec_session_t *session, iec_option_t option, const void *value, uint32_t value_size) {
            return iec104_set_option(session, option, value, value_size);
        });
}

void test_iec104_raw_asdu()
{
    const auto valid = make_iec104_config();
    exercise_raw_asdu(
        "iec104",
        valid,
        [](const iec_session_config_t *common,
           const iec104_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return iec104_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return iec104_destroy(session); },
        [](iec_session_t *session) { return iec104_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return iec104_stop(session, timeout_ms); },
        [](iec_session_t *session, iec_option_t option, const void *value, uint32_t value_size) {
            return iec104_set_option(session, option, value, value_size);
        },
        [](iec_session_t *session, const iec_raw_asdu_tx_t *request) {
            return iec104_send_raw_asdu(session, request);
        });
}

void test_iec104_general_interrogation()
{
    const auto valid = make_iec104_config();
    exercise_general_interrogation(
        "iec104",
        valid,
        [](const iec_session_config_t *common,
           const iec104_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return iec104_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return iec104_destroy(session); },
        [](iec_session_t *session) { return iec104_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return iec104_stop(session, timeout_ms); },
        [](iec_session_t *session, const iec_interrogation_request_t *request) {
            return iec104_general_interrogation(session, request);
        });
}

void test_iec104_counter_interrogation()
{
    const auto valid = make_iec104_config();
    exercise_counter_interrogation(
        "iec104",
        valid,
        [](const iec_session_config_t *common,
           const iec104_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return iec104_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return iec104_destroy(session); },
        [](iec_session_t *session) { return iec104_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return iec104_stop(session, timeout_ms); },
        [](iec_session_t *session, const iec_counter_interrogation_request_t *request) {
            return iec104_counter_interrogation(session, request);
        });
}

void test_iec104_control_point()
{
    const auto valid = make_iec104_config();
    exercise_control_point(
        "iec104",
        valid,
        [](const iec_session_config_t *common,
           const iec104_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return iec104_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return iec104_destroy(session); },
        [](iec_session_t *session) { return iec104_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return iec104_stop(session, timeout_ms); },
        [](iec_session_t *session, const iec_command_request_t *request, uint32_t *out_command_id) {
            return iec104_control_point(session, request, out_command_id);
        });
}

void test_iec104_read_point()
{
    const auto valid = make_iec104_config();
    exercise_read_point(
        "iec104",
        valid,
        [](const iec_session_config_t *common,
           const iec104_master_config_t *config,
           const iec_transport_t *transport,
           const iec_callbacks_t *callbacks,
           iec_session_t **out_session) {
            return iec104_create(common, config, transport, callbacks, out_session);
        },
        [](iec_session_t *session) { return iec104_destroy(session); },
        [](iec_session_t *session) { return iec104_start(session); },
        [](iec_session_t *session, uint32_t timeout_ms) { return iec104_stop(session, timeout_ms); },
        [](iec_session_t *session, const iec_point_address_t *address) {
            return iec104_read_point(session, address);
        });
}

struct TestCase {
    const char *name;
    void (*run)();
};

bool matches_filter(const char *filter, const char *name)
{
    if (filter == nullptr || filter[0] == '\0') {
        return true;
    }
    const std::string requested{filter};
    return requested == "all" || requested == name;
}

} // namespace

int gw_protocol_run_lifecycle_tests(const char *filter)
{
    const TestCase tests[] = {
        {"m101.validate_config", test_m101_validate_config},
        {"m101.lifecycle", test_m101_lifecycle},
        {"m101.raw_asdu", test_m101_raw_asdu},
        {"m101.general_interrogation", test_m101_general_interrogation},
        {"m101.counter_interrogation", test_m101_counter_interrogation},
        {"m101.control_point", test_m101_control_point},
        {"m101.read_point", test_m101_read_point},
        {"iec101.validate_config", test_iec101_validate_config},
        {"iec101.lifecycle", test_iec101_lifecycle},
        {"iec101.raw_asdu", test_iec101_raw_asdu},
        {"iec101.general_interrogation", test_iec101_general_interrogation},
        {"iec101.counter_interrogation", test_iec101_counter_interrogation},
        {"iec101.control_point", test_iec101_control_point},
        {"iec101.read_point", test_iec101_read_point},
        {"iec104.validate_config", test_iec104_validate_config},
        {"iec104.lifecycle", test_iec104_lifecycle},
        {"iec104.raw_asdu", test_iec104_raw_asdu},
        {"iec104.general_interrogation", test_iec104_general_interrogation},
        {"iec104.counter_interrogation", test_iec104_counter_interrogation},
        {"iec104.control_point", test_iec104_control_point},
        {"iec104.read_point", test_iec104_read_point},
    };

    int failures = 0;
    int selected = 0;
    for (const auto &test : tests) {
        if (!matches_filter(filter, test.name)) {
            continue;
        }
        ++selected;
        try {
            test.run();
        } catch (const std::exception &ex) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s\n", test.name, ex.what());
        } catch (...) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: unknown exception\n", test.name);
        }
    }

    if (selected == 0) {
        std::fprintf(stderr, "unknown test case: %s\n", filter == nullptr ? "" : filter);
        std::fprintf(stderr, "available test cases:\n");
        for (const auto &test : tests) {
            std::fprintf(stderr, "  %s\n", test.name);
        }
        return 1;
    }

    return failures;
}
