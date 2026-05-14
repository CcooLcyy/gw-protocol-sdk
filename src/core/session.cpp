#include "core/session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <variant>
#include <vector>

struct iec_session {
    gw::protocol::Profile profile;
    iec_session_config_t config;
    iec_transport_t transport;
    iec_callbacks_t callbacks;
    std::variant<m101_master_config_t, iec101_master_config_t, iec104_master_config_t> protocol_config;
    mutable std::mutex mutex;
    std::thread worker;
    bool stop_requested = false;
    iec_runtime_state_t state = IEC_RUNTIME_CREATED;
};

namespace gw::protocol {
namespace {

struct AsduLayout {
    uint8_t cot_length = 0;
    uint8_t common_address_length = 0;
    uint8_t information_object_address_length = 0;
};

struct PointDecodeResult {
    iec_point_value_t value{};
    uint32_t consumed = 0;
};

constexpr uint8_t kAsduTypeSinglePoint = 1;
constexpr uint8_t kAsduTypeDoublePoint = 3;
constexpr uint8_t kAsduTypeStepPosition = 5;
constexpr uint8_t kAsduTypeBitstring32 = 7;
constexpr uint8_t kAsduTypeMeasuredNormalized = 9;
constexpr uint8_t kAsduTypeMeasuredScaled = 11;
constexpr uint8_t kAsduTypeMeasuredShortFloat = 13;
constexpr uint8_t kAsduTypeIntegratedTotal = 15;
constexpr uint8_t kAsduTypeGeneralInterrogation = 100;
constexpr uint8_t kCauseActivation = 6;
constexpr uint8_t kDefaultOriginatorAddress = 0;
constexpr uint8_t kGeneralInterrogationMinQualifier = 20;
constexpr uint8_t kGeneralInterrogationMaxQualifier = 36;

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

AsduLayout get_asdu_layout(const iec_session_t &session) noexcept
{
    switch (session.profile) {
    case Profile::M101: {
        const auto &config = std::get<m101_master_config_t>(session.protocol_config);
        return AsduLayout{config.cot_length, config.common_address_length, config.information_object_address_length};
    }
    case Profile::IEC101: {
        const auto &config = std::get<iec101_master_config_t>(session.protocol_config);
        return AsduLayout{config.cot_length, config.common_address_length, config.information_object_address_length};
    }
    case Profile::IEC104: {
        const auto &config = std::get<iec104_master_config_t>(session.protocol_config);
        return AsduLayout{config.cot_length, config.common_address_length, config.information_object_address_length};
    }
    }
    return AsduLayout{};
}

uint16_t read_uint16_le(const uint8_t *data, uint8_t length) noexcept
{
    uint16_t value = 0;
    for (uint8_t i = 0; i < length; ++i) {
        value |= static_cast<uint16_t>(data[i]) << (i * 8U);
    }
    return value;
}

uint32_t read_uint32_le(const uint8_t *data, uint8_t length) noexcept
{
    uint32_t value = 0;
    for (uint8_t i = 0; i < length; ++i) {
        value |= static_cast<uint32_t>(data[i]) << (i * 8U);
    }
    return value;
}

int16_t read_int16_le(const uint8_t *data) noexcept
{
    return static_cast<int16_t>(read_uint16_le(data, 2));
}

int32_t read_int32_le(const uint8_t *data) noexcept
{
    return static_cast<int32_t>(read_uint32_le(data, 4));
}

void write_uint_le(uint8_t *buffer, uint32_t &offset, uint32_t value, uint8_t length) noexcept
{
    for (uint8_t i = 0; i < length; ++i) {
        buffer[offset++] = static_cast<uint8_t>((value >> (i * 8U)) & 0xFFU);
    }
}

bool fits_uint_le(uint32_t value, uint8_t length) noexcept
{
    if (length >= sizeof(uint32_t)) {
        return true;
    }
    const uint32_t max_value = (1U << (length * 8U)) - 1U;
    return value <= max_value;
}

uint64_t monotonic_ns() noexcept
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
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

void notify_raw_asdu(
    iec_session_t *session,
    iec_raw_asdu_direction_t direction,
    const uint8_t *payload,
    uint32_t payload_size,
    const AsduLayout &layout,
    iec_on_raw_asdu_fn callback,
    void *user_context) noexcept
{
    if (callback == nullptr || payload == nullptr || payload_size == 0) {
        return;
    }

    iec_raw_asdu_event_t event{};
    event.direction = direction;
    event.payload = payload;
    event.payload_size = payload_size;
    event.monotonic_ns = monotonic_ns();

    if (payload_size >= 2U + layout.cot_length) {
        const uint32_t cause_offset = 2U;
        event.type_id = payload[0];
        event.cause_of_transmission = payload[cause_offset];
        const uint32_t common_address_offset = cause_offset + layout.cot_length;
        if (payload_size >= common_address_offset + layout.common_address_length) {
            event.common_address = read_uint16_le(payload + common_address_offset, layout.common_address_length);
        }
    }

    callback(session, &event, user_context);
}

bool decode_point_value(uint8_t type_id, const uint8_t *data, uint32_t available, PointDecodeResult &out) noexcept
{
    out = PointDecodeResult{};

    switch (type_id) {
    case kAsduTypeSinglePoint:
        if (available < 1) {
            return false;
        }
        out.value.point_type = IEC_POINT_SINGLE;
        out.value.quality = static_cast<uint8_t>(data[0] & 0xF0U);
        out.value.data.single = static_cast<uint8_t>(data[0] & 0x01U);
        out.consumed = 1;
        return true;
    case kAsduTypeDoublePoint:
        if (available < 1) {
            return false;
        }
        out.value.point_type = IEC_POINT_DOUBLE;
        out.value.quality = static_cast<uint8_t>(data[0] & 0xF0U);
        out.value.data.doubled = static_cast<uint8_t>(data[0] & 0x03U);
        out.consumed = 1;
        return true;
    case kAsduTypeStepPosition:
        if (available < 2) {
            return false;
        }
        out.value.point_type = IEC_POINT_STEP;
        out.value.quality = data[1];
        out.value.data.step = static_cast<int8_t>(data[0] & 0x7FU);
        if ((data[0] & 0x40U) != 0) {
            out.value.data.step = static_cast<int8_t>(out.value.data.step | 0x80U);
        }
        out.consumed = 2;
        return true;
    case kAsduTypeBitstring32:
        if (available < 5) {
            return false;
        }
        out.value.point_type = IEC_POINT_BITSTRING32;
        out.value.quality = data[4];
        out.value.data.bitstring32 = read_uint32_le(data, 4);
        out.consumed = 5;
        return true;
    case kAsduTypeMeasuredNormalized:
        if (available < 3) {
            return false;
        }
        out.value.point_type = IEC_POINT_MEASURED_NORMALIZED;
        out.value.quality = data[2];
        out.value.data.normalized = read_int16_le(data);
        out.consumed = 3;
        return true;
    case kAsduTypeMeasuredScaled:
        if (available < 3) {
            return false;
        }
        out.value.point_type = IEC_POINT_MEASURED_SCALED;
        out.value.quality = data[2];
        out.value.data.scaled = read_int16_le(data);
        out.consumed = 3;
        return true;
    case kAsduTypeMeasuredShortFloat:
        if (available < 5) {
            return false;
        }
        out.value.point_type = IEC_POINT_MEASURED_SHORT_FLOAT;
        out.value.quality = data[4];
        std::memcpy(&out.value.data.short_float, data, sizeof(float));
        out.consumed = 5;
        return true;
    case kAsduTypeIntegratedTotal:
        if (available < 5) {
            return false;
        }
        out.value.point_type = IEC_POINT_INTEGRATED_TOTAL;
        out.value.quality = data[4];
        out.value.data.integrated_total = read_int32_le(data);
        out.consumed = 5;
        return true;
    default:
        return false;
    }
}

void dispatch_point_indications(
    iec_session_t *session,
    const uint8_t *payload,
    uint32_t payload_size,
    const AsduLayout &layout,
    iec_on_point_indication_fn callback,
    void *user_context) noexcept
{
    if (callback == nullptr || payload == nullptr || payload_size < 2U + layout.cot_length +
            layout.common_address_length + layout.information_object_address_length) {
        return;
    }

    const uint8_t type_id = payload[0];
    const uint8_t variable_structure = payload[1];
    const uint8_t count = static_cast<uint8_t>(variable_structure & 0x7FU);
    if (count == 0) {
        return;
    }

    const bool is_sequence = (variable_structure & 0x80U) != 0;
    const uint32_t cause_offset = 2U;
    const uint8_t cause = payload[cause_offset];
    const uint8_t originator = layout.cot_length == 2 ? payload[cause_offset + 1] : 0;
    const uint32_t common_address_offset = cause_offset + layout.cot_length;
    const uint16_t common_address =
        read_uint16_le(payload + common_address_offset, layout.common_address_length);
    uint32_t offset = common_address_offset + layout.common_address_length;

    if (is_sequence) {
        if (payload_size < offset + layout.information_object_address_length) {
            return;
        }
        uint32_t information_object_address =
            read_uint32_le(payload + offset, layout.information_object_address_length);
        offset += layout.information_object_address_length;

        for (uint8_t i = 0; i < count; ++i) {
            PointDecodeResult decoded{};
            if (!decode_point_value(type_id, payload + offset, payload_size - offset, decoded)) {
                return;
            }
            decoded.value.is_sequence = 1;

            iec_point_address_t address{};
            address.common_address = common_address;
            address.information_object_address = information_object_address++;
            address.type_id = type_id;
            address.cause_of_transmission = cause;
            address.originator_address = originator;
            callback(session, &address, &decoded.value, user_context);

            offset += decoded.consumed;
        }
        return;
    }

    for (uint8_t i = 0; i < count; ++i) {
        if (payload_size < offset + layout.information_object_address_length) {
            return;
        }
        const uint32_t information_object_address =
            read_uint32_le(payload + offset, layout.information_object_address_length);
        offset += layout.information_object_address_length;

        PointDecodeResult decoded{};
        if (!decode_point_value(type_id, payload + offset, payload_size - offset, decoded)) {
            return;
        }

        iec_point_address_t address{};
        address.common_address = common_address;
        address.information_object_address = information_object_address;
        address.type_id = type_id;
        address.cause_of_transmission = cause;
        address.originator_address = originator;
        callback(session, &address, &decoded.value, user_context);

        offset += decoded.consumed;
    }
}

void receive_worker(iec_session_t *session) noexcept
{
    for (;;) {
        iec_transport_t transport{};
        iec_on_raw_asdu_fn raw_callback = nullptr;
        iec_on_point_indication_fn point_callback = nullptr;
        void *user_context = nullptr;
        uint32_t recv_timeout_ms = 50;
        uint32_t max_frame_len = 0;
        bool raw_enabled = false;
        AsduLayout layout{};

        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->stop_requested || session->state != IEC_RUNTIME_RUNNING) {
                return;
            }
            transport = session->transport;
            raw_callback = session->callbacks.on_raw_asdu;
            point_callback = session->callbacks.on_point_indication;
            user_context = session->config.user_context;
            recv_timeout_ms = std::min<uint32_t>(session->config.command_timeout_ms, 50U);
            if (recv_timeout_ms == 0) {
                recv_timeout_ms = 1;
            }
            max_frame_len = session->transport.max_plain_frame_len;
            raw_enabled = session->config.enable_raw_asdu != 0;
            layout = get_asdu_layout(*session);
        }

        std::vector<uint8_t> buffer;
        try {
            buffer.resize(max_frame_len);
        } catch (...) {
            return;
        }
        uint32_t received = 0;
        const int recv_result =
            transport.recv(transport.ctx, buffer.data(), max_frame_len, &received, recv_timeout_ms);
        if (recv_result != 0 || received == 0 || received > max_frame_len) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->stop_requested || session->state != IEC_RUNTIME_RUNNING) {
                return;
            }
        }

        if (raw_enabled) {
            notify_raw_asdu(
                session,
                IEC_RAW_ASDU_RX,
                buffer.data(),
                received,
                layout,
                raw_callback,
                user_context);
        }
        dispatch_point_indications(
            session,
            buffer.data(),
            received,
            layout,
            point_callback,
            user_context);
    }
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
        session->stop_requested = true;
    }
    if (session->worker.joinable()) {
        session->worker.join();
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

iec_status_t general_interrogation(iec_session_t *session, const iec_interrogation_request_t *request) noexcept
{
    if (session == nullptr || request == nullptr) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    if (request->qualifier < kGeneralInterrogationMinQualifier ||
        request->qualifier > kGeneralInterrogationMaxQualifier) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    uint8_t frame[16]{};
    uint32_t frame_size = 0;
    iec_transport_t transport{};
    iec_on_raw_asdu_fn raw_callback = nullptr;
    void *user_context = nullptr;
    uint32_t timeout_ms = 0;
    bool raw_enabled = false;
    AsduLayout layout{};

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_RUNNING) {
            return IEC_STATUS_BAD_STATE;
        }

        layout = get_asdu_layout(*session);
        if (layout.cot_length == 0 || layout.common_address_length == 0 ||
            layout.information_object_address_length == 0 ||
            !fits_uint_le(request->common_address, layout.common_address_length)) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }

        frame[frame_size++] = kAsduTypeGeneralInterrogation;
        frame[frame_size++] = 1;
        write_uint_le(frame, frame_size, kCauseActivation, 1);
        if (layout.cot_length == 2) {
            write_uint_le(frame, frame_size, kDefaultOriginatorAddress, 1);
        }
        write_uint_le(frame, frame_size, request->common_address, layout.common_address_length);
        write_uint_le(frame, frame_size, 0, layout.information_object_address_length);
        frame[frame_size++] = request->qualifier;

        if (frame_size > session->transport.max_plain_frame_len) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }

        transport = session->transport;
        raw_callback = session->callbacks.on_raw_asdu;
        user_context = session->config.user_context;
        timeout_ms = session->config.command_timeout_ms;
        raw_enabled = session->config.enable_raw_asdu != 0;
    }

    const int send_result = transport.send(transport.ctx, frame, frame_size, timeout_ms);
    if (send_result != 0) {
        return IEC_STATUS_IO_ERROR;
    }

    if (raw_enabled) {
        notify_raw_asdu(
            session,
            IEC_RAW_ASDU_TX,
            frame,
            frame_size,
            layout,
            raw_callback,
            user_context);
    }

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

iec_status_t send_raw_asdu(iec_session_t *session, const iec_raw_asdu_tx_t *request) noexcept
{
    if (session == nullptr || request == nullptr || request->payload == nullptr || request->payload_size == 0 ||
        request->bypass_high_level_validation > 1) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    iec_transport_t transport{};
    iec_on_raw_asdu_fn callback = nullptr;
    void *user_context = nullptr;
    uint32_t timeout_ms = 0;
    AsduLayout layout{};

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_RUNNING || session->config.enable_raw_asdu == 0) {
            return IEC_STATUS_BAD_STATE;
        }
        if (request->payload_size > session->transport.max_plain_frame_len) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }

        layout = get_asdu_layout(*session);
        const uint32_t minimum_asdu_size = 2U + layout.cot_length + layout.common_address_length;
        if (layout.cot_length == 0 || layout.common_address_length == 0 ||
            request->payload_size < minimum_asdu_size) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }

        transport = session->transport;
        callback = session->callbacks.on_raw_asdu;
        user_context = session->config.user_context;
        timeout_ms = session->config.command_timeout_ms;
    }

    const int send_result = transport.send(transport.ctx, request->payload, request->payload_size, timeout_ms);
    if (send_result != 0) {
        return IEC_STATUS_IO_ERROR;
    }

    if (callback != nullptr) {
        notify_raw_asdu(
            session,
            IEC_RAW_ASDU_TX,
            request->payload,
            request->payload_size,
            layout,
            callback,
            user_context);
    }

    return IEC_STATUS_OK;
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

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->stop_requested = false;
        session->state = IEC_RUNTIME_RUNNING;
    }

    try {
        session->worker = std::thread(receive_worker, session);
    } catch (const std::bad_alloc &) {
        change_state(session, IEC_RUNTIME_FAULTED);
        return IEC_STATUS_NO_MEMORY;
    } catch (...) {
        change_state(session, IEC_RUNTIME_FAULTED);
        return IEC_STATUS_INTERNAL_ERROR;
    }

    notify_state(session, IEC_RUNTIME_RUNNING);
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
        session->stop_requested = true;
    }
    change_state(session, IEC_RUNTIME_STOPPING);
    if (session->worker.joinable()) {
        session->worker.join();
    }
    change_state(session, IEC_RUNTIME_STOPPED);
    return IEC_STATUS_OK;
}

} // namespace gw::protocol
