#include "core/session.hpp"

#include <cstdint>
#include <mutex>
#include <new>
#include <variant>

struct iec_session {
    gw::protocol::Profile profile;
    iec_session_config_t config;
    iec_transport_t transport;
    iec_callbacks_t callbacks;
    std::variant<m101_master_config_t, iec101_master_config_t, iec104_master_config_t> protocol_config;
    mutable std::mutex mutex;
    iec_runtime_state_t state = IEC_RUNTIME_CREATED;
};

namespace gw::protocol {
namespace {

bool is_link_mode_valid(iec101_link_mode_t mode) noexcept
{
    return mode == IEC101_LINK_MODE_UNBALANCED || mode == IEC101_LINK_MODE_BALANCED;
}

bool is_binary_flag(uint8_t value) noexcept
{
    return value == 0 || value == 1;
}

bool is_length(uint8_t value, uint8_t min, uint8_t max) noexcept
{
    return value >= min && value <= max;
}

iec_status_t validate_101_common(const iec101_master_config_t &config) noexcept
{
    if (!is_link_mode_valid(config.link_mode)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    if (!is_length(config.link_address_length, 1, 2)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    if (!is_length(config.common_address_length, 1, 2)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    if (!is_length(config.information_object_address_length, 1, 3)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    if (!is_length(config.cot_length, 1, 2)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    if (!is_binary_flag(config.use_single_char_ack)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    if (config.ack_timeout_ms == 0 || config.repeat_timeout_ms == 0 || config.repeat_count == 0) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    return IEC_STATUS_OK;
}

bool is_transport_valid(const iec_transport_t &transport) noexcept
{
    return transport.send != nullptr && transport.recv != nullptr && transport.max_plain_frame_len > 0;
}

bool is_session_config_valid(const iec_session_config_t &config) noexcept
{
    if (!is_binary_flag(config.enable_raw_asdu) || !is_binary_flag(config.enable_log_callback)) {
        return false;
    }
    if (config.initial_log_level != 0 &&
        (config.initial_log_level < IEC_LOG_ERROR || config.initial_log_level > IEC_LOG_DEBUG)) {
        return false;
    }
    return true;
}

bool read_option_value(const void *value, uint32_t value_size, uint32_t &out) noexcept
{
    if (value == nullptr) {
        return false;
    }
    if (value_size == sizeof(uint8_t)) {
        out = *static_cast<const uint8_t *>(value);
        return true;
    }
    if (value_size == sizeof(uint16_t)) {
        out = *static_cast<const uint16_t *>(value);
        return true;
    }
    if (value_size == sizeof(uint32_t)) {
        out = *static_cast<const uint32_t *>(value);
        return true;
    }
    return false;
}

void notify_state(iec_session_t *session, iec_runtime_state_t state) noexcept
{
    iec_on_session_state_fn callback = nullptr;
    void *user_context = nullptr;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        callback = session->callbacks.on_session_state;
        user_context = session->config.user_context;
    }
    if (callback != nullptr) {
        callback(session, state, user_context);
    }
}

iec_status_t change_state(iec_session_t *session, iec_runtime_state_t state) noexcept
{
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->state = state;
    }
    notify_state(session, state);
    return IEC_STATUS_OK;
}

} // namespace

iec_status_t validate_iec101_config(const iec101_master_config_t *config) noexcept
{
    if (config == nullptr) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    return validate_101_common(*config);
}

iec_status_t validate_m101_config(const m101_master_config_t *config) noexcept
{
    if (config == nullptr) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    iec101_master_config_t common{
        config->link_mode,
        config->link_address,
        config->link_address_length,
        config->common_address_length,
        config->information_object_address_length,
        config->cot_length,
        config->use_single_char_ack,
        config->ack_timeout_ms,
        config->repeat_timeout_ms,
        config->repeat_count,
    };
    const iec_status_t status = validate_101_common(common);
    if (status != IEC_STATUS_OK) {
        return status;
    }
    if (config->preferred_file_chunk_size == 0 || config->preferred_file_chunk_size > 1024) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    return IEC_STATUS_OK;
}

iec_status_t validate_iec104_config(const iec104_master_config_t *config) noexcept
{
    if (config == nullptr) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    if (!is_length(config->common_address_length, 1, 2)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    if (!is_length(config->information_object_address_length, 1, 3)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    if (!is_length(config->cot_length, 1, 2)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    if (config->k == 0 || config->w == 0 || config->t0_ms == 0 || config->t1_ms == 0 ||
        config->t2_ms == 0 || config->t3_ms == 0) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    return IEC_STATUS_OK;
}

iec_status_t create_session(
    Profile profile,
    const iec_session_config_t *config,
    const void *protocol_config,
    const iec_transport_t *transport,
    const iec_callbacks_t *callbacks,
    iec_session_t **out_session) noexcept
{
    if (out_session != nullptr) {
        *out_session = nullptr;
    }
    if (config == nullptr || protocol_config == nullptr || transport == nullptr || callbacks == nullptr ||
        out_session == nullptr) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    if (!is_session_config_valid(*config) || !is_transport_valid(*transport)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    try {
        auto *session = new iec_session{};
        session->profile = profile;
        session->config = *config;
        session->transport = *transport;
        session->callbacks = *callbacks;
        session->state = IEC_RUNTIME_CREATED;

        switch (profile) {
        case Profile::M101: {
            const auto *typed_config = static_cast<const m101_master_config_t *>(protocol_config);
            const iec_status_t status = validate_m101_config(typed_config);
            if (status != IEC_STATUS_OK) {
                delete session;
                return status;
            }
            session->protocol_config = *typed_config;
            break;
        }
        case Profile::IEC101: {
            const auto *typed_config = static_cast<const iec101_master_config_t *>(protocol_config);
            const iec_status_t status = validate_iec101_config(typed_config);
            if (status != IEC_STATUS_OK) {
                delete session;
                return status;
            }
            session->protocol_config = *typed_config;
            break;
        }
        case Profile::IEC104: {
            const auto *typed_config = static_cast<const iec104_master_config_t *>(protocol_config);
            const iec_status_t status = validate_iec104_config(typed_config);
            if (status != IEC_STATUS_OK) {
                delete session;
                return status;
            }
            session->protocol_config = *typed_config;
            break;
        }
        }

        *out_session = session;
        return IEC_STATUS_OK;
    } catch (const std::bad_alloc &) {
        return IEC_STATUS_NO_MEMORY;
    } catch (...) {
        return IEC_STATUS_INTERNAL_ERROR;
    }
}

iec_status_t destroy_session(iec_session_t *session) noexcept
{
    if (session == nullptr) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_CREATED && session->state != IEC_RUNTIME_STOPPED &&
            session->state != IEC_RUNTIME_FAULTED) {
            return IEC_STATUS_BAD_STATE;
        }
    }
    delete session;
    return IEC_STATUS_OK;
}

iec_status_t get_runtime_state(const iec_session_t *session, iec_runtime_state_t *out_state) noexcept
{
    if (session == nullptr || out_state == nullptr) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    *out_state = session->state;
    return IEC_STATUS_OK;
}

iec_status_t set_option(iec_session_t *session, iec_option_t option, const void *value, uint32_t value_size) noexcept
{
    if (session == nullptr) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    uint32_t parsed = 0;
    if (!read_option_value(value, value_size, parsed)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    std::lock_guard<std::mutex> lock(session->mutex);
    switch (option) {
    case IEC_OPTION_LOG_LEVEL:
        if (parsed < IEC_LOG_ERROR || parsed > IEC_LOG_DEBUG) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }
        session->config.initial_log_level = static_cast<uint8_t>(parsed);
        return IEC_STATUS_OK;
    case IEC_OPTION_RECONNECT_INTERVAL_MS:
        session->config.reconnect_interval_ms = parsed;
        return IEC_STATUS_OK;
    case IEC_OPTION_COMMAND_TIMEOUT_MS:
        session->config.command_timeout_ms = parsed;
        return IEC_STATUS_OK;
    case IEC_OPTION_ENABLE_RAW_ASDU:
        if (parsed > 1) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }
        session->config.enable_raw_asdu = static_cast<uint8_t>(parsed);
        return IEC_STATUS_OK;
    default:
        return IEC_STATUS_INVALID_ARGUMENT;
    }
}

iec_status_t start_session(iec_session_t *session) noexcept
{
    if (session == nullptr) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_CREATED) {
            return IEC_STATUS_BAD_STATE;
        }
    }
    change_state(session, IEC_RUNTIME_STARTING);
    change_state(session, IEC_RUNTIME_RUNNING);
    return IEC_STATUS_OK;
}

iec_status_t stop_session(iec_session_t *session, uint32_t /*timeout_ms*/) noexcept
{
    if (session == nullptr) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_RUNNING) {
            return IEC_STATUS_BAD_STATE;
        }
    }
    change_state(session, IEC_RUNTIME_STOPPING);
    change_state(session, IEC_RUNTIME_STOPPED);
    return IEC_STATUS_OK;
}

} // namespace gw::protocol
