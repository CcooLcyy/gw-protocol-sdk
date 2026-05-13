#include "gw_protocol_sdk.h"

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
};

int mock_send(void *ctx, const uint8_t *data, uint32_t len, uint32_t timeout_ms)
{
    (void)data;
    (void)len;
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
    return 0;
}

int mock_recv(void *ctx, uint8_t *buffer, uint32_t capacity, uint32_t *out_len, uint32_t timeout_ms)
{
    (void)buffer;
    (void)capacity;

    auto *transport = static_cast<MockTransport *>(ctx);
    if (transport == nullptr || out_len == nullptr) {
        return -1;
    }

    const uint32_t sleep_ms = std::min<uint32_t>(timeout_ms == 0 ? 1U : timeout_ms, 5U);
    std::unique_lock<std::mutex> lock(transport->mutex);
    transport->cv.wait_for(lock, std::chrono::milliseconds(sleep_ms), [&] { return transport->closed; });
    if (transport->closed) {
        return -1;
    }

    *out_len = 0;
    ++transport->recv_count;
    return 0;
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

    void record(iec_runtime_state_t state)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            states.push_back(state);
        }
        cv.notify_all();
    }

    std::vector<iec_runtime_state_t> snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return states;
    }

    bool wait_until_contains(iec_runtime_state_t state)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, kStateWaitTimeout, [&] {
            return std::find(states.begin(), states.end(), state) != states.end();
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
        EXPECT_STATUS(
            set_option(session, IEC_OPTION_ENABLE_RAW_ASDU, &raw_asdu_enabled, sizeof(raw_asdu_enabled)),
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

void test_iec101()
{
    auto valid = make_iec101_config();
    auto invalid = valid;
    invalid.common_address_length = 0;

    exercise_validate_config(
        "iec101",
        valid,
        [](const iec101_master_config_t *config) { return iec101_validate_config(config); },
        invalid);

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

void test_m101()
{
    auto valid = make_m101_config();
    auto invalid = valid;
    invalid.common_address_length = 0;

    exercise_validate_config(
        "m101",
        valid,
        [](const m101_master_config_t *config) { return m101_validate_config(config); },
        invalid);

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

void test_iec104()
{
    auto valid = make_iec104_config();
    auto invalid = valid;
    invalid.common_address_length = 0;

    exercise_validate_config(
        "iec104",
        valid,
        [](const iec104_master_config_t *config) { return iec104_validate_config(config); },
        invalid);

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

struct TestCase {
    const char *name;
    void (*run)();
};

} // namespace

int gw_protocol_run_lifecycle_tests()
{
    const TestCase tests[] = {
        {"m101", test_m101},
        {"iec101", test_iec101},
        {"iec104", test_iec104},
    };

    int failures = 0;
    for (const auto &test : tests) {
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

    return failures;
}
