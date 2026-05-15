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

    struct ClockEvent {
        iec_clock_result_t result{};
    };

    std::vector<ClockEvent> clock_events;

    struct ParameterIndicationEvent {
        iec_parameter_indication_t indication{};
        std::string value_text;
    };

    std::vector<ParameterIndicationEvent> parameter_indications;

    struct ParameterResultEvent {
        iec_parameter_result_t result{};
    };

    std::vector<ParameterResultEvent> parameter_results;

    struct FileListEvent {
        iec_file_list_indication_t indication{};
        std::string directory_name;
        std::vector<iec_file_entry_t> entries;
        std::vector<std::string> entry_directories;
        std::vector<std::string> entry_names;
        std::vector<std::string> checksums;
    };

    std::vector<FileListEvent> file_lists;

    struct FileDataEvent {
        iec_file_data_indication_t indication{};
        std::string directory_name;
        std::string file_name;
        std::vector<uint8_t> data;
    };

    std::vector<FileDataEvent> file_data;

    struct FileResultEvent {
        iec_file_operation_result_t result{};
        std::string directory_name;
        std::string file_name;
        std::string detail_message;
    };

    std::vector<FileResultEvent> file_results;

    void record(iec_runtime_state_t state)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            states.push_back(state);
        }
        cv.notify_all();
    }

    void record_clock(const iec_clock_result_t &result)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            clock_events.push_back(ClockEvent{result});
        }
        cv.notify_all();
    }

    void record_parameter_indication(const iec_parameter_indication_t &indication)
    {
        ParameterIndicationEvent snapshot{};
        snapshot.indication = indication;
        if (indication.item.value_type == IEC_PARAMETER_VALUE_STRING &&
            indication.item.value.string_value != nullptr) {
            snapshot.value_text = indication.item.value.string_value;
            snapshot.indication.item.value.string_value = snapshot.value_text.c_str();
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            parameter_indications.push_back(std::move(snapshot));
        }
        cv.notify_all();
    }

    void record_parameter_result(const iec_parameter_result_t &result)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            parameter_results.push_back(ParameterResultEvent{result});
        }
        cv.notify_all();
    }

    void record_file_list(const iec_file_list_indication_t &indication)
    {
        FileListEvent snapshot{};
        snapshot.indication = indication;
        if (indication.directory_name != nullptr) {
            snapshot.directory_name = indication.directory_name;
            snapshot.indication.directory_name = snapshot.directory_name.c_str();
        }
        if (indication.entries != nullptr && indication.entry_count > 0) {
            snapshot.entries.assign(indication.entries, indication.entries + indication.entry_count);
            snapshot.entry_directories.resize(indication.entry_count);
            snapshot.entry_names.resize(indication.entry_count);
            snapshot.checksums.resize(indication.entry_count);
            for (uint32_t i = 0; i < indication.entry_count; ++i) {
                if (indication.entries[i].directory_name != nullptr) {
                    snapshot.entry_directories[i] = indication.entries[i].directory_name;
                    snapshot.entries[i].directory_name = snapshot.entry_directories[i].c_str();
                }
                if (indication.entries[i].file_name != nullptr) {
                    snapshot.entry_names[i] = indication.entries[i].file_name;
                    snapshot.entries[i].file_name = snapshot.entry_names[i].c_str();
                }
                if (indication.entries[i].checksum_text != nullptr) {
                    snapshot.checksums[i] = indication.entries[i].checksum_text;
                    snapshot.entries[i].checksum_text = snapshot.checksums[i].c_str();
                }
            }
            snapshot.indication.entries = snapshot.entries.data();
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            file_lists.push_back(std::move(snapshot));
        }
        cv.notify_all();
    }

    void record_file_data(const iec_file_data_indication_t &indication)
    {
        FileDataEvent snapshot{};
        snapshot.indication = indication;
        if (indication.directory_name != nullptr) {
            snapshot.directory_name = indication.directory_name;
            snapshot.indication.directory_name = snapshot.directory_name.c_str();
        }
        if (indication.file_name != nullptr) {
            snapshot.file_name = indication.file_name;
            snapshot.indication.file_name = snapshot.file_name.c_str();
        }
        if (indication.data != nullptr && indication.data_size > 0) {
            snapshot.data.assign(indication.data, indication.data + indication.data_size);
            snapshot.indication.data = snapshot.data.data();
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            file_data.push_back(std::move(snapshot));
        }
        cv.notify_all();
    }

    void record_file_result(const iec_file_operation_result_t &result)
    {
        FileResultEvent snapshot{};
        snapshot.result = result;
        if (result.directory_name != nullptr) {
            snapshot.directory_name = result.directory_name;
            snapshot.result.directory_name = snapshot.directory_name.c_str();
        }
        if (result.file_name != nullptr) {
            snapshot.file_name = result.file_name;
            snapshot.result.file_name = snapshot.file_name.c_str();
        }
        if (result.detail_message != nullptr) {
            snapshot.detail_message = result.detail_message;
            snapshot.result.detail_message = snapshot.detail_message.c_str();
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            file_results.push_back(std::move(snapshot));
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

    std::vector<ClockEvent> clock_snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return clock_events;
    }

    std::vector<ParameterIndicationEvent> parameter_indication_snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return parameter_indications;
    }

    std::vector<ParameterResultEvent> parameter_result_snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return parameter_results;
    }

    std::vector<FileListEvent> file_list_snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return file_lists;
    }

    std::vector<FileDataEvent> file_data_snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return file_data;
    }

    std::vector<FileResultEvent> file_result_snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return file_results;
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

    bool wait_until_clock_count(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, kStateWaitTimeout, [&] {
            return clock_events.size() >= count;
        });
    }

    bool wait_until_parameter_indication_count(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, kStateWaitTimeout, [&] {
            return parameter_indications.size() >= count;
        });
    }

    bool wait_until_parameter_result_count(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, kStateWaitTimeout, [&] {
            return parameter_results.size() >= count;
        });
    }

    bool wait_until_file_list_count(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, kStateWaitTimeout, [&] {
            return file_lists.size() >= count;
        });
    }

    bool wait_until_file_data_count(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, kStateWaitTimeout, [&] {
            return file_data.size() >= count;
        });
    }

    bool wait_until_file_result_count(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, kStateWaitTimeout, [&] {
            return file_results.size() >= count;
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

void on_clock_result(iec_session_t *session, const iec_clock_result_t *result, void *user_context)
{
    (void)session;
    auto *recorder = static_cast<StateRecorder *>(user_context);
    if (recorder == nullptr || result == nullptr) {
        return;
    }
    recorder->record_clock(*result);
}

void on_parameter_indication(
    iec_session_t *session,
    const iec_parameter_indication_t *indication,
    void *user_context)
{
    (void)session;
    auto *recorder = static_cast<StateRecorder *>(user_context);
    if (recorder == nullptr || indication == nullptr) {
        return;
    }
    recorder->record_parameter_indication(*indication);
}

void on_parameter_result(iec_session_t *session, const iec_parameter_result_t *result, void *user_context)
{
    (void)session;
    auto *recorder = static_cast<StateRecorder *>(user_context);
    if (recorder == nullptr || result == nullptr) {
        return;
    }
    recorder->record_parameter_result(*result);
}

void on_file_list_indication(
    iec_session_t *session,
    const iec_file_list_indication_t *indication,
    void *user_context)
{
    (void)session;
    auto *recorder = static_cast<StateRecorder *>(user_context);
    if (recorder == nullptr || indication == nullptr) {
        return;
    }
    recorder->record_file_list(*indication);
}

void on_file_data_indication(
    iec_session_t *session,
    const iec_file_data_indication_t *indication,
    void *user_context)
{
    (void)session;
    auto *recorder = static_cast<StateRecorder *>(user_context);
    if (recorder == nullptr || indication == nullptr) {
        return;
    }
    recorder->record_file_data(*indication);
}

void on_file_operation_result(
    iec_session_t *session,
    const iec_file_operation_result_t *result,
    void *user_context)
{
    (void)session;
    auto *recorder = static_cast<StateRecorder *>(user_context);
    if (recorder == nullptr || result == nullptr) {
        return;
    }
    recorder->record_file_result(*result);
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
    callbacks.on_clock_result = on_clock_result;
    callbacks.on_parameter_indication = on_parameter_indication;
    callbacks.on_parameter_result = on_parameter_result;
    callbacks.on_file_list_indication = on_file_list_indication;
    callbacks.on_file_data_indication = on_file_data_indication;
    callbacks.on_file_operation_result = on_file_operation_result;
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

template <
    typename Config,
    typename CreateFn,
    typename DestroyFn,
    typename StartFn,
    typename StopFn,
    typename ClockSyncFn,
    typename ReadClockFn>
void exercise_clock(
    const char *name,
    const Config &protocol_config,
    CreateFn create,
    DestroyFn destroy,
    StartFn start,
    StopFn stop,
    ClockSyncFn clock_sync,
    ReadClockFn read_clock)
{
    StateRecorder recorder;
    MockTransport mock;
    auto common = make_common_config(recorder);
    auto transport = make_transport(mock);
    auto callbacks = make_callbacks();

    iec_session_t *session = nullptr;
    iec_clock_sync_request_t sync_request{};
    sync_request.common_address = 1;
    sync_request.use_current_system_time = 0;
    sync_request.timestamp.msec = 1234;
    sync_request.timestamp.minute = 56;
    sync_request.timestamp.hour = 7;
    sync_request.timestamp.day = 8;
    sync_request.timestamp.month = 9;
    sync_request.timestamp.year = 26;

    auto invalid_sync = sync_request;
    invalid_sync.timestamp.minute = 60;
    auto invalid_flag = sync_request;
    invalid_flag.use_current_system_time = 2;

    const iec_clock_read_request_t read_request{1};
    const std::vector<uint8_t> expected_sync_request{
        103,
        1,
        6,
        0,
        1,
        0,
        0,
        0,
        0,
        0xd2,
        0x04,
        56,
        7,
        8,
        9,
        26,
    };
    const std::vector<uint8_t> expected_read_request{
        102,
        1,
        5,
        0,
        1,
        0,
        0,
        0,
        0,
    };
    const std::vector<uint8_t> sync_response{
        103,
        1,
        7,
        0,
        1,
        0,
        0,
        0,
        0,
        0xd2,
        0x04,
        56,
        7,
        8,
        9,
        26,
    };
    const std::vector<uint8_t> read_response{
        103,
        1,
        5,
        0,
        1,
        0,
        0,
        0,
        0,
        0x39,
        0x30,
        34,
        12,
        15,
        5,
        26,
    };

    uint32_t request_id = 99;
    EXPECT_STATUS(clock_sync(nullptr, &sync_request, &request_id), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_TRUE(request_id == 0);
    EXPECT_STATUS(read_clock(nullptr, &read_request, &request_id), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_TRUE(request_id == 0);
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
        request_id = 99;
        EXPECT_STATUS(clock_sync(session, nullptr, &request_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_TRUE(request_id == 0);
        EXPECT_STATUS(clock_sync(session, &sync_request, nullptr), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(clock_sync(session, &invalid_flag, &request_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(clock_sync(session, &invalid_sync, &request_id), IEC_STATUS_BAD_STATE);
        EXPECT_STATUS(read_clock(session, nullptr, &request_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_TRUE(request_id == 0);
        EXPECT_STATUS(read_clock(session, &read_request, nullptr), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(clock_sync(session, &sync_request, &request_id), IEC_STATUS_BAD_STATE);
        EXPECT_TRUE(request_id == 0);
        EXPECT_STATUS(read_clock(session, &read_request, &request_id), IEC_STATUS_BAD_STATE);
        EXPECT_TRUE(request_id == 0);

        EXPECT_STATUS(start(session), IEC_STATUS_OK);
        EXPECT_TRUE(recorder.wait_until_contains(IEC_RUNTIME_RUNNING));
        EXPECT_STATUS(clock_sync(session, &invalid_sync, &request_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(clock_sync(session, &sync_request, &request_id), IEC_STATUS_OK);
        EXPECT_TRUE(request_id == 1);
        EXPECT_TRUE(mock.send_count == 1);
        EXPECT_TRUE(mock.last_sent == expected_sync_request);

        EXPECT_STATUS(read_clock(session, &read_request, &request_id), IEC_STATUS_OK);
        EXPECT_TRUE(request_id == 2);
        EXPECT_TRUE(mock.send_count == 2);
        EXPECT_TRUE(mock.last_sent == expected_read_request);

        push_recv(mock, sync_response);
        EXPECT_TRUE(recorder.wait_until_clock_count(1));
        push_recv(mock, read_response);
        EXPECT_TRUE(recorder.wait_until_clock_count(2));

        const auto clocks = recorder.clock_snapshot();
        EXPECT_TRUE(clocks.size() == 2);
        EXPECT_TRUE(clocks[0].result.request_id == 1);
        EXPECT_TRUE(clocks[0].result.operation == IEC_CLOCK_OPERATION_SYNC);
        EXPECT_TRUE(clocks[0].result.result == IEC_CLOCK_RESULT_ACCEPTED);
        EXPECT_TRUE(clocks[0].result.common_address == 1);
        EXPECT_TRUE(clocks[0].result.has_timestamp == 0);
        EXPECT_TRUE(clocks[0].result.cause_of_transmission == 7);
        EXPECT_TRUE(clocks[1].result.request_id == 2);
        EXPECT_TRUE(clocks[1].result.operation == IEC_CLOCK_OPERATION_READ);
        EXPECT_TRUE(clocks[1].result.result == IEC_CLOCK_RESULT_ACCEPTED);
        EXPECT_TRUE(clocks[1].result.common_address == 1);
        EXPECT_TRUE(clocks[1].result.has_timestamp == 1);
        EXPECT_TRUE(clocks[1].result.timestamp.msec == 12345);
        EXPECT_TRUE(clocks[1].result.timestamp.minute == 34);
        EXPECT_TRUE(clocks[1].result.timestamp.hour == 12);
        EXPECT_TRUE(clocks[1].result.timestamp.day == 15);
        EXPECT_TRUE(clocks[1].result.timestamp.month == 5);
        EXPECT_TRUE(clocks[1].result.timestamp.year == 26);
        EXPECT_TRUE(clocks[1].result.timestamp.invalid == 0);
        EXPECT_TRUE(clocks[1].result.cause_of_transmission == 5);

        EXPECT_STATUS(stop(session, 1000), IEC_STATUS_OK);
        EXPECT_STATUS(destroy(session), IEC_STATUS_OK);
        session = nullptr;
        close_transport(mock);
    } catch (...) {
        cleanup();
        throw;
    }

    std::printf("  %s clock\n", name);
}

template <
    typename Config,
    typename CreateFn,
    typename DestroyFn,
    typename StartFn,
    typename StopFn,
    typename ReadParametersFn,
    typename WriteParametersFn,
    typename VerifyParametersFn,
    typename SwitchSettingGroupFn>
void exercise_parameters(
    const char *name,
    const Config &protocol_config,
    CreateFn create,
    DestroyFn destroy,
    StartFn start,
    StopFn stop,
    ReadParametersFn read_parameters,
    WriteParametersFn write_parameters,
    VerifyParametersFn verify_parameters,
    SwitchSettingGroupFn switch_setting_group)
{
    StateRecorder recorder;
    MockTransport mock;
    auto common = make_common_config(recorder);
    auto transport = make_transport(mock);
    auto callbacks = make_callbacks();

    iec_session_t *session = nullptr;
    iec_parameter_read_request_t read_request{};
    read_request.common_address = 1;
    read_request.read_mode = IEC_PARAMETER_READ_BY_ADDRESS_RANGE;
    read_request.scope = IEC_PARAMETER_SCOPE_RUNNING;
    read_request.start_address = 0x1000;
    read_request.end_address = 0x1001;
    read_request.setting_group = 2;
    read_request.include_descriptor = 1;

    iec_parameter_item_t write_items[2]{};
    write_items[0].parameter_id = 10;
    write_items[0].address = 0x1000;
    write_items[0].scope = IEC_PARAMETER_SCOPE_RUNNING;
    write_items[0].value_type = IEC_PARAMETER_VALUE_UINT;
    write_items[0].value.uint_value = 1234;
    write_items[1].parameter_id = 11;
    write_items[1].address = 0x1001;
    write_items[1].scope = IEC_PARAMETER_SCOPE_RUNNING;
    write_items[1].value_type = IEC_PARAMETER_VALUE_BOOL;
    write_items[1].value.bool_value = 1;

    iec_parameter_write_request_t write_request{};
    write_request.common_address = 1;
    write_request.setting_group = 2;
    write_request.items = write_items;
    write_request.item_count = 2;
    write_request.verify_after_write = 1;

    iec_parameter_verify_request_t verify_request{};
    verify_request.common_address = 1;
    verify_request.setting_group = 2;
    verify_request.expected_items = write_items;
    verify_request.item_count = 2;

    iec_setting_group_request_t get_group{};
    get_group.common_address = 1;
    get_group.action = IEC_SETTING_GROUP_ACTION_GET_CURRENT;
    get_group.target_group = 0;

    iec_setting_group_request_t switch_group{};
    switch_group.common_address = 1;
    switch_group.action = IEC_SETTING_GROUP_ACTION_SWITCH;
    switch_group.target_group = 3;

    const std::vector<uint8_t> expected_read_request{
        202, 1, 5, 0, 1, 0, 0, 0, 0, 2, 2, 4, 2, 0x00, 0x10, 0, 0, 0x01, 0x10, 0, 0, 0,
    };
    const std::vector<uint8_t> expected_write_request{
        203, 1, 6, 0, 1, 0, 0, 0, 0, 2, 2, 2,
        10, 0, 0, 0, 0x00, 0x10, 0, 0, 2, 3, 4, 0xd2, 0x04, 0, 0,
        11, 0, 0, 0, 0x01, 0x10, 0, 0, 2, 1, 1, 1,
    };
    const std::vector<uint8_t> expected_verify_request{
        204, 1, 6, 0, 1, 0, 0, 0, 0, 2, 0, 2,
        10, 0, 0, 0, 0x00, 0x10, 0, 0, 2, 3, 4, 0xd2, 0x04, 0, 0,
        11, 0, 0, 0, 0x01, 0x10, 0, 0, 2, 1, 1, 1,
    };
    const std::vector<uint8_t> expected_get_group_request{
        205, 1, 6, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0,
    };
    const std::vector<uint8_t> expected_switch_group_request{
        205, 1, 6, 0, 1, 0, 0, 0, 0, 3, 0, 2, 3,
    };
    const std::vector<uint8_t> read_response{
        202, 1, 10, 0, 1, 0, 0, 0, 0, 2, 3,
        10, 0, 0, 0, 0x00, 0x10, 0, 0, 2, 3, 4, 0xd2, 0x04, 0, 0,
    };
    const std::vector<uint8_t> write_response{
        203, 1, 7, 0, 1, 0, 0, 0, 0, 2, 1, 10, 0, 0, 0, 0x00, 0x10, 0, 0,
    };
    const std::vector<uint8_t> verify_response{
        204, 1, 7, 0, 1, 0, 0, 0, 0, 2, 1, 10, 0, 0, 0, 0x00, 0x10, 0, 0,
    };
    const std::vector<uint8_t> current_group_response{
        205, 1, 5, 0, 1, 0, 0, 0, 0, 2, 1,
    };
    const std::vector<uint8_t> switch_group_response{
        205, 1, 7, 0, 1, 0, 0, 0, 0, 3, 1,
    };

    uint32_t request_id = 99;
    EXPECT_STATUS(read_parameters(nullptr, &read_request, &request_id), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_TRUE(request_id == 0);
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
        auto invalid_read = read_request;
        invalid_read.start_address = 2;
        invalid_read.end_address = 1;
        auto invalid_write = write_request;
        invalid_write.verify_after_write = 2;
        auto invalid_item = write_items[0];
        invalid_item.value_type = IEC_PARAMETER_VALUE_STRING;
        invalid_item.value.string_value = nullptr;
        iec_parameter_write_request_t invalid_items_request = write_request;
        invalid_items_request.items = &invalid_item;
        invalid_items_request.item_count = 1;
        auto invalid_switch = switch_group;
        invalid_switch.target_group = 0;

        EXPECT_STATUS(read_parameters(session, nullptr, &request_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(read_parameters(session, &invalid_read, &request_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(write_parameters(session, &invalid_write, &request_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(write_parameters(session, &invalid_items_request, &request_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(verify_parameters(session, nullptr, &request_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(switch_setting_group(session, &invalid_switch, &request_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(read_parameters(session, &read_request, &request_id), IEC_STATUS_BAD_STATE);
        EXPECT_TRUE(request_id == 0);

        EXPECT_STATUS(start(session), IEC_STATUS_OK);
        EXPECT_TRUE(recorder.wait_until_contains(IEC_RUNTIME_RUNNING));

        EXPECT_STATUS(read_parameters(session, &read_request, &request_id), IEC_STATUS_OK);
        EXPECT_TRUE(request_id == 1);
        EXPECT_TRUE(mock.last_sent == expected_read_request);
        push_recv(mock, read_response);
        EXPECT_TRUE(recorder.wait_until_parameter_indication_count(1));
        const auto indications = recorder.parameter_indication_snapshot();
        EXPECT_TRUE(indications.size() == 1);
        EXPECT_TRUE(indications[0].indication.request_id == 1);
        EXPECT_TRUE(indications[0].indication.operation == IEC_PARAMETER_OPERATION_READ);
        EXPECT_TRUE(indications[0].indication.setting_group == 2);
        EXPECT_TRUE(indications[0].indication.is_final == 1);
        EXPECT_TRUE(indications[0].indication.has_descriptor == 1);
        EXPECT_TRUE(indications[0].indication.item.parameter_id == 10);
        EXPECT_TRUE(indications[0].indication.item.address == 0x1000);
        EXPECT_TRUE(indications[0].indication.item.value_type == IEC_PARAMETER_VALUE_UINT);
        EXPECT_TRUE(indications[0].indication.item.value.uint_value == 1234);

        EXPECT_STATUS(write_parameters(session, &write_request, &request_id), IEC_STATUS_OK);
        EXPECT_TRUE(request_id == 2);
        EXPECT_TRUE(mock.last_sent == expected_write_request);
        push_recv(mock, write_response);
        EXPECT_TRUE(recorder.wait_until_parameter_result_count(1));

        EXPECT_STATUS(verify_parameters(session, &verify_request, &request_id), IEC_STATUS_OK);
        EXPECT_TRUE(request_id == 3);
        EXPECT_TRUE(mock.last_sent == expected_verify_request);
        push_recv(mock, verify_response);
        EXPECT_TRUE(recorder.wait_until_parameter_result_count(2));

        EXPECT_STATUS(switch_setting_group(session, &get_group, &request_id), IEC_STATUS_OK);
        EXPECT_TRUE(request_id == 4);
        EXPECT_TRUE(mock.last_sent == expected_get_group_request);
        push_recv(mock, current_group_response);
        EXPECT_TRUE(recorder.wait_until_parameter_result_count(3));

        EXPECT_STATUS(switch_setting_group(session, &switch_group, &request_id), IEC_STATUS_OK);
        EXPECT_TRUE(request_id == 5);
        EXPECT_TRUE(mock.last_sent == expected_switch_group_request);
        push_recv(mock, switch_group_response);
        EXPECT_TRUE(recorder.wait_until_parameter_result_count(4));

        const auto results = recorder.parameter_result_snapshot();
        EXPECT_TRUE(results.size() == 4);
        EXPECT_TRUE(results[0].result.request_id == 2);
        EXPECT_TRUE(results[0].result.operation == IEC_PARAMETER_OPERATION_WRITE);
        EXPECT_TRUE(results[0].result.result == IEC_PARAMETER_RESULT_ACCEPTED);
        EXPECT_TRUE(results[0].result.parameter_id == 10);
        EXPECT_TRUE(results[1].result.request_id == 3);
        EXPECT_TRUE(results[1].result.operation == IEC_PARAMETER_OPERATION_VERIFY);
        EXPECT_TRUE(results[1].result.result == IEC_PARAMETER_RESULT_VERIFY_OK);
        EXPECT_TRUE(results[2].result.request_id == 4);
        EXPECT_TRUE(results[2].result.operation == IEC_PARAMETER_OPERATION_SWITCH_GROUP);
        EXPECT_TRUE(results[2].result.result == IEC_PARAMETER_RESULT_CURRENT_GROUP);
        EXPECT_TRUE(results[2].result.setting_group == 2);
        EXPECT_TRUE(results[3].result.request_id == 5);
        EXPECT_TRUE(results[3].result.operation == IEC_PARAMETER_OPERATION_SWITCH_GROUP);
        EXPECT_TRUE(results[3].result.result == IEC_PARAMETER_RESULT_GROUP_SWITCHED);
        EXPECT_TRUE(results[3].result.setting_group == 3);

        EXPECT_STATUS(stop(session, 1000), IEC_STATUS_OK);
        EXPECT_STATUS(destroy(session), IEC_STATUS_OK);
        session = nullptr;
        close_transport(mock);
    } catch (...) {
        cleanup();
        throw;
    }

    std::printf("  %s parameters\n", name);
}

template <
    typename Config,
    typename CreateFn,
    typename DestroyFn,
    typename StartFn,
    typename StopFn,
    typename ListFilesFn,
    typename ReadFileFn,
    typename WriteFileFn,
    typename GetFileTransferStatusFn,
    typename CancelFileTransferFn>
void exercise_file_transfer(
    const char *name,
    const Config &protocol_config,
    CreateFn create,
    DestroyFn destroy,
    StartFn start,
    StopFn stop,
    ListFilesFn list_files,
    ReadFileFn read_file,
    WriteFileFn write_file,
    GetFileTransferStatusFn get_file_transfer_status,
    CancelFileTransferFn cancel_file_transfer)
{
    StateRecorder recorder;
    MockTransport mock;
    auto common = make_common_config(recorder);
    auto transport = make_transport(mock);
    transport.max_plain_frame_len = 512;
    auto callbacks = make_callbacks();

    iec_session_t *session = nullptr;
    const iec_file_list_request_t list_request{1, "/cfg", 1};
    const iec_file_read_request_t read_request{1, "/cfg", "terminal.xml", 4, 64, 16};
    const uint8_t content[] = {0xaa, 0xbb, 0xcc, 0xdd};
    const iec_file_write_request_t write_request{
        1,
        "/cfg",
        "record.dat",
        4,
        16,
        content,
        static_cast<uint32_t>(sizeof(content)),
        64,
        1,
    };

    const std::vector<uint8_t> expected_list_request{
        206, 1, 5, 0, 1, 0, 0, 0, 0, 2, 0, 4, '/', 'c', 'f', 'g',
    };
    const std::vector<uint8_t> expected_read_request{
        207, 1, 6, 0, 1, 0, 0, 0, 0, 0, 0,
        4, '/', 'c', 'f', 'g',
        12, 't', 'e', 'r', 'm', 'i', 'n', 'a', 'l', '.', 'x', 'm', 'l',
        1, 0, 0, 0,
        4, 0, 0, 0,
        64, 0, 0, 0,
        16, 0, 0, 0,
    };
    const std::vector<uint8_t> expected_write_request{
        208, 1, 6, 0, 1, 0, 0, 0, 0, 2, 0,
        4, '/', 'c', 'f', 'g',
        10, 'r', 'e', 'c', 'o', 'r', 'd', '.', 'd', 'a', 't',
        2, 0, 0, 0,
        4, 0, 0, 0,
        16, 0, 0, 0,
        64, 0, 0, 0,
        4, 0, 0, 0,
        0xaa, 0xbb, 0xcc, 0xdd,
    };
    const std::vector<uint8_t> expected_cancel_request{
        209, 1, 6, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
    };

    const std::vector<uint8_t> list_response{
        206, 1, 10, 0, 1, 0, 0, 0, 0, 1, 2,
        4, '/', 'c', 'f', 'g',
        1,
        4, '/', 'c', 'f', 'g',
        12, 't', 'e', 'r', 'm', 'i', 'n', 'a', 'l', '.', 'x', 'm', 'l',
        16, 0, 0, 0,
        0x88, 0x77, 0x66, 0x55,
        0x44, 0x33, 0x22, 0x11,
        0, 1,
        3, 'm', 'd', '5',
    };
    const std::vector<uint8_t> read_response{
        207, 1, 10, 0, 1, 0, 0, 0, 0, 1, 2,
        1, 0, 0, 0,
        4, 0, 0, 0,
        8, 0, 0, 0,
        16, 0, 0, 0,
        4, 0, 0, 0,
        1, 2, 3, 4,
    };
    const std::vector<uint8_t> write_response{
        208, 1, 10, 0, 1, 0, 0, 0, 0, 1, 2,
        2, 0, 0, 0,
        8, 0, 0, 0,
        16, 0, 0, 0,
    };

    uint32_t request_id = 99;
    uint32_t transfer_id = 99;
    iec_file_transfer_status_t status{};
    EXPECT_STATUS(list_files(nullptr, &list_request, &request_id), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_TRUE(request_id == 0);
    EXPECT_STATUS(read_file(nullptr, &read_request, &transfer_id), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_TRUE(transfer_id == 0);
    EXPECT_STATUS(get_file_transfer_status(nullptr, 1, &status), IEC_STATUS_INVALID_ARGUMENT);
    EXPECT_STATUS(cancel_file_transfer(nullptr, 1), IEC_STATUS_INVALID_ARGUMENT);
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
        auto invalid_list = list_request;
        invalid_list.directory_name = "";
        auto invalid_read = read_request;
        invalid_read.file_name = nullptr;
        auto invalid_write = write_request;
        invalid_write.content_size = 0;

        EXPECT_STATUS(list_files(session, nullptr, &request_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(list_files(session, &invalid_list, &request_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(read_file(session, &invalid_read, &transfer_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(write_file(session, &invalid_write, &transfer_id), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(get_file_transfer_status(session, 0, &status), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(cancel_file_transfer(session, 0), IEC_STATUS_INVALID_ARGUMENT);
        EXPECT_STATUS(list_files(session, &list_request, &request_id), IEC_STATUS_BAD_STATE);
        EXPECT_TRUE(request_id == 0);

        EXPECT_STATUS(start(session), IEC_STATUS_OK);
        EXPECT_TRUE(recorder.wait_until_contains(IEC_RUNTIME_RUNNING));

        EXPECT_STATUS(list_files(session, &list_request, &request_id), IEC_STATUS_OK);
        EXPECT_TRUE(request_id == 1);
        EXPECT_TRUE(mock.last_sent == expected_list_request);
        push_recv(mock, list_response);
        EXPECT_TRUE(recorder.wait_until_file_list_count(1));
        EXPECT_TRUE(recorder.wait_until_file_result_count(1));
        const auto lists = recorder.file_list_snapshot();
        EXPECT_TRUE(lists.size() == 1);
        EXPECT_TRUE(lists[0].indication.request_id == 1);
        EXPECT_TRUE(lists[0].directory_name == "/cfg");
        EXPECT_TRUE(lists[0].indication.entry_count == 1);
        EXPECT_TRUE(lists[0].entry_names[0] == "terminal.xml");
        EXPECT_TRUE(lists[0].entries[0].file_size == 16);
        EXPECT_TRUE(lists[0].entries[0].is_read_only == 1);
        EXPECT_TRUE(lists[0].checksums[0] == "md5");

        EXPECT_STATUS(read_file(session, &read_request, &transfer_id), IEC_STATUS_OK);
        EXPECT_TRUE(transfer_id == 1);
        EXPECT_TRUE(mock.last_sent == expected_read_request);
        EXPECT_STATUS(get_file_transfer_status(session, transfer_id, &status), IEC_STATUS_OK);
        EXPECT_TRUE(status.transfer_id == 1);
        EXPECT_TRUE(status.direction == IEC_FILE_TRANSFER_DIRECTION_READ);
        EXPECT_TRUE(status.state == IEC_FILE_TRANSFER_STATE_ACCEPTED);
        EXPECT_TRUE(std::string(status.directory_name) == "/cfg");
        EXPECT_TRUE(std::string(status.file_name) == "terminal.xml");
        EXPECT_TRUE(status.acknowledged_offset == 4);

        push_recv(mock, read_response);
        EXPECT_TRUE(recorder.wait_until_file_data_count(1));
        EXPECT_STATUS(get_file_transfer_status(session, transfer_id, &status), IEC_STATUS_OK);
        EXPECT_TRUE(status.state == IEC_FILE_TRANSFER_STATE_COMPLETED);
        EXPECT_TRUE(status.acknowledged_offset == 8);
        const auto data = recorder.file_data_snapshot();
        EXPECT_TRUE(data.size() == 1);
        EXPECT_TRUE(data[0].indication.transfer_id == 1);
        EXPECT_TRUE(data[0].indication.current_offset == 4);
        EXPECT_TRUE(data[0].indication.next_offset == 8);
        EXPECT_TRUE(data[0].data == std::vector<uint8_t>({1, 2, 3, 4}));

        EXPECT_STATUS(write_file(session, &write_request, &transfer_id), IEC_STATUS_OK);
        EXPECT_TRUE(transfer_id == 2);
        EXPECT_TRUE(mock.last_sent == expected_write_request);
        push_recv(mock, write_response);
        EXPECT_TRUE(recorder.wait_until_file_result_count(3));
        EXPECT_STATUS(get_file_transfer_status(session, transfer_id, &status), IEC_STATUS_OK);
        EXPECT_TRUE(status.direction == IEC_FILE_TRANSFER_DIRECTION_WRITE);
        EXPECT_TRUE(status.state == IEC_FILE_TRANSFER_STATE_COMPLETED);
        EXPECT_TRUE(status.acknowledged_offset == 8);

        const iec_file_read_request_t cancel_read{1, "/cfg", "cancel.bin", 0, 64, 32};
        EXPECT_STATUS(read_file(session, &cancel_read, &transfer_id), IEC_STATUS_OK);
        EXPECT_TRUE(transfer_id == 3);
        EXPECT_STATUS(cancel_file_transfer(session, transfer_id), IEC_STATUS_OK);
        auto expected_cancel_for_third = expected_cancel_request;
        expected_cancel_for_third[11] = 3;
        EXPECT_TRUE(mock.last_sent == expected_cancel_for_third);
        EXPECT_TRUE(recorder.wait_until_file_result_count(4));
        EXPECT_STATUS(get_file_transfer_status(session, transfer_id, &status), IEC_STATUS_OK);
        EXPECT_TRUE(status.state == IEC_FILE_TRANSFER_STATE_CANCELED);
        EXPECT_TRUE(status.last_result == IEC_FILE_RESULT_CANCELED);

        const auto results = recorder.file_result_snapshot();
        EXPECT_TRUE(results.size() == 4);
        EXPECT_TRUE(results[0].result.request_id == 1);
        EXPECT_TRUE(results[0].result.operation == IEC_FILE_OPERATION_LIST);
        EXPECT_TRUE(results[0].result.result == IEC_FILE_RESULT_COMPLETED);
        EXPECT_TRUE(results[1].result.transfer_id == 1);
        EXPECT_TRUE(results[1].result.operation == IEC_FILE_OPERATION_READ);
        EXPECT_TRUE(results[1].result.result == IEC_FILE_RESULT_COMPLETED);
        EXPECT_TRUE(results[2].result.transfer_id == 2);
        EXPECT_TRUE(results[2].result.operation == IEC_FILE_OPERATION_WRITE);
        EXPECT_TRUE(results[2].result.result == IEC_FILE_RESULT_COMPLETED);
        EXPECT_TRUE(results[3].result.transfer_id == 3);
        EXPECT_TRUE(results[3].result.operation == IEC_FILE_OPERATION_CANCEL);
        EXPECT_TRUE(results[3].result.result == IEC_FILE_RESULT_CANCELED);

        EXPECT_STATUS(stop(session, 1000), IEC_STATUS_OK);
        EXPECT_STATUS(destroy(session), IEC_STATUS_OK);
        session = nullptr;
        close_transport(mock);
    } catch (...) {
        cleanup();
        throw;
    }

    std::printf("  %s file_transfer\n", name);
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

void test_iec101_clock()
{
    const auto valid = make_iec101_config();
    exercise_clock(
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
        [](iec_session_t *session, const iec_clock_sync_request_t *request, uint32_t *out_request_id) {
            return iec101_clock_sync(session, request, out_request_id);
        },
        [](iec_session_t *session, const iec_clock_read_request_t *request, uint32_t *out_request_id) {
            return iec101_read_clock(session, request, out_request_id);
        });
}

void test_iec101_parameters()
{
    const auto valid = make_iec101_config();
    exercise_parameters(
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
        [](iec_session_t *session, const iec_parameter_read_request_t *request, uint32_t *out_request_id) {
            return iec101_read_parameters(session, request, out_request_id);
        },
        [](iec_session_t *session, const iec_parameter_write_request_t *request, uint32_t *out_request_id) {
            return iec101_write_parameters(session, request, out_request_id);
        },
        [](iec_session_t *session, const iec_parameter_verify_request_t *request, uint32_t *out_request_id) {
            return iec101_verify_parameters(session, request, out_request_id);
        },
        [](iec_session_t *session, const iec_setting_group_request_t *request, uint32_t *out_request_id) {
            return iec101_switch_setting_group(session, request, out_request_id);
        });
}

void test_iec101_file_transfer()
{
    const auto valid = make_iec101_config();
    exercise_file_transfer(
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
        [](iec_session_t *session, const iec_file_list_request_t *request, uint32_t *out_request_id) {
            return iec101_list_files(session, request, out_request_id);
        },
        [](iec_session_t *session, const iec_file_read_request_t *request, uint32_t *out_transfer_id) {
            return iec101_read_file(session, request, out_transfer_id);
        },
        [](iec_session_t *session, const iec_file_write_request_t *request, uint32_t *out_transfer_id) {
            return iec101_write_file(session, request, out_transfer_id);
        },
        [](const iec_session_t *session, uint32_t transfer_id, iec_file_transfer_status_t *out_status) {
            return iec101_get_file_transfer_status(session, transfer_id, out_status);
        },
        [](iec_session_t *session, uint32_t transfer_id) {
            return iec101_cancel_file_transfer(session, transfer_id);
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

void test_m101_clock()
{
    const auto valid = make_m101_config();
    exercise_clock(
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
        [](iec_session_t *session, const iec_clock_sync_request_t *request, uint32_t *out_request_id) {
            return m101_clock_sync(session, request, out_request_id);
        },
        [](iec_session_t *session, const iec_clock_read_request_t *request, uint32_t *out_request_id) {
            return m101_read_clock(session, request, out_request_id);
        });
}

void test_m101_parameters()
{
    const auto valid = make_m101_config();
    exercise_parameters(
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
        [](iec_session_t *session, const iec_parameter_read_request_t *request, uint32_t *out_request_id) {
            return m101_read_parameters(session, request, out_request_id);
        },
        [](iec_session_t *session, const iec_parameter_write_request_t *request, uint32_t *out_request_id) {
            return m101_write_parameters(session, request, out_request_id);
        },
        [](iec_session_t *session, const iec_parameter_verify_request_t *request, uint32_t *out_request_id) {
            return m101_verify_parameters(session, request, out_request_id);
        },
        [](iec_session_t *session, const iec_setting_group_request_t *request, uint32_t *out_request_id) {
            return m101_switch_setting_group(session, request, out_request_id);
        });
}

void test_m101_file_transfer()
{
    const auto valid = make_m101_config();
    exercise_file_transfer(
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
        [](iec_session_t *session, const iec_file_list_request_t *request, uint32_t *out_request_id) {
            return m101_list_files(session, request, out_request_id);
        },
        [](iec_session_t *session, const iec_file_read_request_t *request, uint32_t *out_transfer_id) {
            return m101_read_file(session, request, out_transfer_id);
        },
        [](iec_session_t *session, const iec_file_write_request_t *request, uint32_t *out_transfer_id) {
            return m101_write_file(session, request, out_transfer_id);
        },
        [](const iec_session_t *session, uint32_t transfer_id, iec_file_transfer_status_t *out_status) {
            return m101_get_file_transfer_status(session, transfer_id, out_status);
        },
        [](iec_session_t *session, uint32_t transfer_id) {
            return m101_cancel_file_transfer(session, transfer_id);
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

void test_iec104_clock()
{
    const auto valid = make_iec104_config();
    exercise_clock(
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
        [](iec_session_t *session, const iec_clock_sync_request_t *request, uint32_t *out_request_id) {
            return iec104_clock_sync(session, request, out_request_id);
        },
        [](iec_session_t *session, const iec_clock_read_request_t *request, uint32_t *out_request_id) {
            return iec104_read_clock(session, request, out_request_id);
        });
}

void test_iec104_parameters()
{
    const auto valid = make_iec104_config();
    exercise_parameters(
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
        [](iec_session_t *session, const iec_parameter_read_request_t *request, uint32_t *out_request_id) {
            return iec104_read_parameters(session, request, out_request_id);
        },
        [](iec_session_t *session, const iec_parameter_write_request_t *request, uint32_t *out_request_id) {
            return iec104_write_parameters(session, request, out_request_id);
        },
        [](iec_session_t *session, const iec_parameter_verify_request_t *request, uint32_t *out_request_id) {
            return iec104_verify_parameters(session, request, out_request_id);
        },
        [](iec_session_t *session, const iec_setting_group_request_t *request, uint32_t *out_request_id) {
            return iec104_switch_setting_group(session, request, out_request_id);
        });
}

void test_iec104_file_transfer()
{
    const auto valid = make_iec104_config();
    exercise_file_transfer(
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
        [](iec_session_t *session, const iec_file_list_request_t *request, uint32_t *out_request_id) {
            return iec104_list_files(session, request, out_request_id);
        },
        [](iec_session_t *session, const iec_file_read_request_t *request, uint32_t *out_transfer_id) {
            return iec104_read_file(session, request, out_transfer_id);
        },
        [](iec_session_t *session, const iec_file_write_request_t *request, uint32_t *out_transfer_id) {
            return iec104_write_file(session, request, out_transfer_id);
        },
        [](const iec_session_t *session, uint32_t transfer_id, iec_file_transfer_status_t *out_status) {
            return iec104_get_file_transfer_status(session, transfer_id, out_status);
        },
        [](iec_session_t *session, uint32_t transfer_id) {
            return iec104_cancel_file_transfer(session, transfer_id);
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
        {"m101.clock", test_m101_clock},
        {"m101.parameters", test_m101_parameters},
        {"m101.file_transfer", test_m101_file_transfer},
        {"iec101.validate_config", test_iec101_validate_config},
        {"iec101.lifecycle", test_iec101_lifecycle},
        {"iec101.raw_asdu", test_iec101_raw_asdu},
        {"iec101.general_interrogation", test_iec101_general_interrogation},
        {"iec101.counter_interrogation", test_iec101_counter_interrogation},
        {"iec101.control_point", test_iec101_control_point},
        {"iec101.read_point", test_iec101_read_point},
        {"iec101.clock", test_iec101_clock},
        {"iec101.parameters", test_iec101_parameters},
        {"iec101.file_transfer", test_iec101_file_transfer},
        {"iec104.validate_config", test_iec104_validate_config},
        {"iec104.lifecycle", test_iec104_lifecycle},
        {"iec104.raw_asdu", test_iec104_raw_asdu},
        {"iec104.general_interrogation", test_iec104_general_interrogation},
        {"iec104.counter_interrogation", test_iec104_counter_interrogation},
        {"iec104.control_point", test_iec104_control_point},
        {"iec104.read_point", test_iec104_read_point},
        {"iec104.clock", test_iec104_clock},
        {"iec104.parameters", test_iec104_parameters},
        {"iec104.file_transfer", test_iec104_file_transfer},
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
