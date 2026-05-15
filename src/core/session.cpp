#include "core/session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <variant>
#include <vector>

struct iec_session {
    struct PendingCommand {
        uint32_t command_id = 0;
        iec_command_semantic_t semantic = IEC_COMMAND_SEMANTIC_GENERAL;
        iec_point_address_t address{};
    };

    struct PendingClock {
        uint32_t request_id = 0;
        iec_clock_operation_t operation = IEC_CLOCK_OPERATION_SYNC;
        uint16_t common_address = 0;
    };

    struct PendingParameter {
        uint32_t request_id = 0;
        iec_parameter_operation_t operation = IEC_PARAMETER_OPERATION_READ;
        uint16_t common_address = 0;
        uint8_t setting_group = 0;
    };

    gw::protocol::Profile profile;
    iec_session_config_t config;
    iec_transport_t transport;
    iec_callbacks_t callbacks;
    std::variant<m101_master_config_t, iec101_master_config_t, iec104_master_config_t> protocol_config;
    mutable std::mutex mutex;
    std::thread worker;
    std::vector<PendingCommand> pending_commands;
    std::vector<PendingClock> pending_clocks;
    std::vector<PendingParameter> pending_parameters;
    bool stop_requested = false;
    iec_runtime_state_t state = IEC_RUNTIME_CREATED;
    uint32_t next_command_id = 1;
    uint32_t next_request_id = 1;
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
constexpr uint8_t kAsduTypeSingleCommand = 45;
constexpr uint8_t kAsduTypeDoubleCommand = 46;
constexpr uint8_t kAsduTypeStepCommand = 47;
constexpr uint8_t kAsduTypeSetpointNormalized = 48;
constexpr uint8_t kAsduTypeSetpointScaled = 49;
constexpr uint8_t kAsduTypeSetpointFloat = 50;
constexpr uint8_t kAsduTypeGeneralInterrogation = 100;
constexpr uint8_t kAsduTypeCounterInterrogation = 101;
constexpr uint8_t kAsduTypeReadCommand = 102;
constexpr uint8_t kAsduTypeClockSync = 103;
constexpr uint8_t kAsduTypeParameterRead = 202;
constexpr uint8_t kAsduTypeParameterWrite = 203;
constexpr uint8_t kAsduTypeParameterVerify = 204;
constexpr uint8_t kAsduTypeSettingGroup = 205;
constexpr uint8_t kCauseRequest = 5;
constexpr uint8_t kCauseActivation = 6;
constexpr uint8_t kCauseActivationConfirm = 7;
constexpr uint8_t kCauseActivationTermination = 10;
constexpr uint8_t kDefaultOriginatorAddress = 0;
constexpr uint8_t kGeneralInterrogationMinQualifier = 20;
constexpr uint8_t kGeneralInterrogationMaxQualifier = 36;
constexpr uint8_t kCounterInterrogationMinQualifier = 1;
constexpr uint8_t kCounterInterrogationMaxQualifier = 5;
constexpr uint8_t kCounterInterrogationMaxFreeze = 3;
constexpr uint8_t kSelectExecuteQualifierMask = 0x80;
constexpr uint32_t kClockSyncInformationObjectAddress = 0;
constexpr uint32_t kParameterChannelInformationObjectAddress = 0;
constexpr uint32_t kMaxParameterItemsPerRequest = 32;
constexpr uint32_t kMaxParameterStringBytes = 64;

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

bool is_command_type_valid(iec_command_type_t type) noexcept
{
    switch (type) {
    case IEC_COMMAND_SINGLE:
    case IEC_COMMAND_DOUBLE:
    case IEC_COMMAND_STEP:
    case IEC_COMMAND_SETPOINT_SCALED:
    case IEC_COMMAND_SETPOINT_FLOAT:
    case IEC_COMMAND_SETPOINT_NORMALIZED:
        return true;
    default:
        return false;
    }
}

bool is_command_semantic_valid(iec_command_semantic_t semantic) noexcept
{
    switch (semantic) {
    case IEC_COMMAND_SEMANTIC_GENERAL:
    case IEC_COMMAND_SEMANTIC_FACTORY_RESET:
    case IEC_COMMAND_SEMANTIC_DEVICE_REBOOT:
        return true;
    default:
        return false;
    }
}

bool is_command_mode_valid(iec_command_mode_t mode) noexcept
{
    switch (mode) {
    case IEC_COMMAND_MODE_DIRECT:
    case IEC_COMMAND_MODE_SELECT:
    case IEC_COMMAND_MODE_EXECUTE:
    case IEC_COMMAND_MODE_CANCEL:
        return true;
    default:
        return false;
    }
}

bool is_command_value_valid(const iec_command_request_t &request) noexcept
{
    switch (request.command_type) {
    case IEC_COMMAND_SINGLE:
        return request.value.single <= 1 && request.qualifier <= 0x1F;
    case IEC_COMMAND_DOUBLE:
        return request.value.doubled <= 3 && request.qualifier <= 0x1F;
    case IEC_COMMAND_STEP:
    case IEC_COMMAND_SETPOINT_NORMALIZED:
    case IEC_COMMAND_SETPOINT_SCALED:
    case IEC_COMMAND_SETPOINT_FLOAT:
        return request.qualifier <= 0x7F;
    default:
        return false;
    }
}

bool is_parameter_scope_valid(iec_parameter_scope_t scope) noexcept
{
    switch (scope) {
    case IEC_PARAMETER_SCOPE_ALL:
    case IEC_PARAMETER_SCOPE_FIXED:
    case IEC_PARAMETER_SCOPE_RUNNING:
    case IEC_PARAMETER_SCOPE_ACTION:
    case IEC_PARAMETER_SCOPE_WIRELESS:
    case IEC_PARAMETER_SCOPE_POWER:
    case IEC_PARAMETER_SCOPE_LINE_LOSS:
    case IEC_PARAMETER_SCOPE_POINT_TABLE:
        return true;
    default:
        return false;
    }
}

bool is_parameter_value_type_valid(iec_parameter_value_type_t type) noexcept
{
    switch (type) {
    case IEC_PARAMETER_VALUE_BOOL:
    case IEC_PARAMETER_VALUE_INT:
    case IEC_PARAMETER_VALUE_UINT:
    case IEC_PARAMETER_VALUE_FLOAT:
    case IEC_PARAMETER_VALUE_ENUM:
    case IEC_PARAMETER_VALUE_STRING:
        return true;
    default:
        return false;
    }
}

bool is_parameter_read_mode_valid(iec_parameter_read_mode_t mode) noexcept
{
    switch (mode) {
    case IEC_PARAMETER_READ_ALL:
    case IEC_PARAMETER_READ_BY_SCOPE:
    case IEC_PARAMETER_READ_BY_GROUP:
    case IEC_PARAMETER_READ_BY_ADDRESS_RANGE:
        return true;
    default:
        return false;
    }
}

bool is_setting_group_action_valid(iec_setting_group_action_t action) noexcept
{
    switch (action) {
    case IEC_SETTING_GROUP_ACTION_GET_CURRENT:
    case IEC_SETTING_GROUP_ACTION_SWITCH:
        return true;
    default:
        return false;
    }
}

bool is_parameter_item_valid(const iec_parameter_item_t &item) noexcept
{
    if (!is_parameter_scope_valid(item.scope) || !is_parameter_value_type_valid(item.value_type)) {
        return false;
    }
    switch (item.value_type) {
    case IEC_PARAMETER_VALUE_BOOL:
        return item.value.bool_value <= 1;
    case IEC_PARAMETER_VALUE_STRING:
        return item.value.string_value != nullptr &&
            std::strlen(item.value.string_value) <= kMaxParameterStringBytes;
    case IEC_PARAMETER_VALUE_INT:
    case IEC_PARAMETER_VALUE_UINT:
    case IEC_PARAMETER_VALUE_FLOAT:
    case IEC_PARAMETER_VALUE_ENUM:
        return true;
    default:
        return false;
    }
}

bool validate_parameter_read_request(const iec_parameter_read_request_t &request) noexcept
{
    if (!is_parameter_read_mode_valid(request.read_mode) || !is_parameter_scope_valid(request.scope) ||
        !is_binary_flag(request.include_descriptor)) {
        return false;
    }
    if (request.read_mode == IEC_PARAMETER_READ_BY_GROUP &&
        (request.group_name == nullptr || request.group_name[0] == '\0' ||
            std::strlen(request.group_name) > kMaxParameterStringBytes)) {
        return false;
    }
    if (request.read_mode == IEC_PARAMETER_READ_BY_ADDRESS_RANGE &&
        request.start_address > request.end_address) {
        return false;
    }
    return true;
}

bool validate_parameter_items(const iec_parameter_item_t *items, uint32_t item_count) noexcept
{
    if (items == nullptr || item_count == 0 || item_count > kMaxParameterItemsPerRequest) {
        return false;
    }
    for (uint32_t i = 0; i < item_count; ++i) {
        if (!is_parameter_item_valid(items[i])) {
            return false;
        }
    }
    return true;
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

void write_int16_le(uint8_t *buffer, uint32_t &offset, int16_t value) noexcept
{
    write_uint_le(buffer, offset, static_cast<uint16_t>(value), 2);
}

void write_float_le(uint8_t *buffer, uint32_t &offset, float value) noexcept
{
    uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    write_uint_le(buffer, offset, raw, 4);
}

void write_int32_le(uint8_t *buffer, uint32_t &offset, int32_t value) noexcept
{
    write_uint_le(buffer, offset, static_cast<uint32_t>(value), 4);
}

bool write_cp56_time2a(uint8_t *buffer, uint32_t &offset, const iec_timestamp_t &timestamp) noexcept
{
    if (timestamp.msec > 59999 || timestamp.minute > 59 || timestamp.hour > 23 || timestamp.day < 1 ||
        timestamp.day > 31 || timestamp.month < 1 || timestamp.month > 12 || timestamp.year > 99 ||
        timestamp.invalid > 1) {
        return false;
    }

    write_uint_le(buffer, offset, timestamp.msec, 2);
    buffer[offset++] = static_cast<uint8_t>(timestamp.minute | (timestamp.invalid != 0 ? 0x80U : 0U));
    buffer[offset++] = timestamp.hour;
    buffer[offset++] = timestamp.day;
    buffer[offset++] = timestamp.month;
    buffer[offset++] = timestamp.year;
    return true;
}

bool read_cp56_time2a(const uint8_t *data, uint32_t available, iec_timestamp_t &out) noexcept
{
    if (data == nullptr || available < 7) {
        return false;
    }

    out = iec_timestamp_t{};
    out.msec = read_uint16_le(data, 2);
    out.minute = static_cast<uint8_t>(data[2] & 0x3FU);
    out.invalid = static_cast<uint8_t>((data[2] & 0x80U) != 0 ? 1U : 0U);
    out.hour = static_cast<uint8_t>(data[3] & 0x1FU);
    out.day = static_cast<uint8_t>(data[4] & 0x1FU);
    out.month = static_cast<uint8_t>(data[5] & 0x0FU);
    out.year = static_cast<uint8_t>(data[6] & 0x7FU);
    return out.msec <= 59999 && out.minute <= 59 && out.hour <= 23 && out.day >= 1 && out.day <= 31 &&
        out.month >= 1 && out.month <= 12 && out.year <= 99;
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

iec_timestamp_t current_system_timestamp() noexcept
{
    iec_timestamp_t timestamp{};
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
#if defined(_WIN32)
    localtime_s(&local_time, &seconds);
#else
    localtime_r(&seconds, &local_time);
#endif
    timestamp.msec =
        static_cast<uint16_t>(local_time.tm_sec * 1000 + static_cast<int>(milliseconds % 1000));
    timestamp.minute = static_cast<uint8_t>(local_time.tm_min);
    timestamp.hour = static_cast<uint8_t>(local_time.tm_hour);
    timestamp.day = static_cast<uint8_t>(local_time.tm_mday);
    timestamp.month = static_cast<uint8_t>(local_time.tm_mon + 1);
    timestamp.year = static_cast<uint8_t>((local_time.tm_year + 1900) % 100);
    timestamp.invalid = 0;
    return timestamp;
}

uint32_t take_next_request_id(iec_session_t &session) noexcept
{
    const uint32_t request_id = session.next_request_id++;
    if (session.next_request_id == 0) {
        session.next_request_id = 1;
    }
    return request_id;
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

bool command_type_id(iec_command_type_t type, uint8_t &out_type_id) noexcept
{
    switch (type) {
    case IEC_COMMAND_SINGLE:
        out_type_id = kAsduTypeSingleCommand;
        return true;
    case IEC_COMMAND_DOUBLE:
        out_type_id = kAsduTypeDoubleCommand;
        return true;
    case IEC_COMMAND_STEP:
        out_type_id = kAsduTypeStepCommand;
        return true;
    case IEC_COMMAND_SETPOINT_NORMALIZED:
        out_type_id = kAsduTypeSetpointNormalized;
        return true;
    case IEC_COMMAND_SETPOINT_SCALED:
        out_type_id = kAsduTypeSetpointScaled;
        return true;
    case IEC_COMMAND_SETPOINT_FLOAT:
        out_type_id = kAsduTypeSetpointFloat;
        return true;
    default:
        return false;
    }
}

bool append_command_value(const iec_command_request_t &request, uint8_t *frame, uint32_t &frame_size) noexcept
{
    switch (request.command_type) {
    case IEC_COMMAND_SINGLE:
        frame[frame_size++] = static_cast<uint8_t>(
            (request.value.single & 0x01U) |
            (request.mode == IEC_COMMAND_MODE_SELECT ? kSelectExecuteQualifierMask : 0U) |
            ((request.qualifier & 0x1FU) << 2U));
        return true;
    case IEC_COMMAND_DOUBLE:
        frame[frame_size++] = static_cast<uint8_t>(
            (request.value.doubled & 0x03U) |
            (request.mode == IEC_COMMAND_MODE_SELECT ? kSelectExecuteQualifierMask : 0U) |
            ((request.qualifier & 0x1FU) << 2U));
        return true;
    case IEC_COMMAND_STEP:
        frame[frame_size++] = static_cast<uint8_t>(request.value.step);
        frame[frame_size++] = static_cast<uint8_t>(
            (request.mode == IEC_COMMAND_MODE_SELECT ? kSelectExecuteQualifierMask : 0U) |
            (request.qualifier & 0x1FU));
        return true;
    case IEC_COMMAND_SETPOINT_NORMALIZED:
        write_int16_le(frame, frame_size, request.value.normalized);
        frame[frame_size++] = static_cast<uint8_t>(
            (request.mode == IEC_COMMAND_MODE_SELECT ? kSelectExecuteQualifierMask : 0U) |
            (request.qualifier & 0x1FU));
        return true;
    case IEC_COMMAND_SETPOINT_SCALED:
        write_int16_le(frame, frame_size, request.value.scaled);
        frame[frame_size++] = static_cast<uint8_t>(
            (request.mode == IEC_COMMAND_MODE_SELECT ? kSelectExecuteQualifierMask : 0U) |
            (request.qualifier & 0x1FU));
        return true;
    case IEC_COMMAND_SETPOINT_FLOAT:
        write_float_le(frame, frame_size, request.value.short_float);
        frame[frame_size++] = static_cast<uint8_t>(
            (request.mode == IEC_COMMAND_MODE_SELECT ? kSelectExecuteQualifierMask : 0U) |
            (request.qualifier & 0x1FU));
        return true;
    default:
        return false;
    }
}

bool append_parameter_value(
    const iec_parameter_item_t &item,
    uint8_t *frame,
    uint32_t &frame_size,
    uint32_t capacity) noexcept
{
    if (capacity < frame_size + 14) {
        return false;
    }

    write_uint_le(frame, frame_size, item.parameter_id, 4);
    write_uint_le(frame, frame_size, item.address, 4);
    frame[frame_size++] = static_cast<uint8_t>(item.scope);
    frame[frame_size++] = static_cast<uint8_t>(item.value_type);

    switch (item.value_type) {
    case IEC_PARAMETER_VALUE_BOOL:
        frame[frame_size++] = 1;
        frame[frame_size++] = item.value.bool_value;
        return true;
    case IEC_PARAMETER_VALUE_INT:
        frame[frame_size++] = 4;
        write_int32_le(frame, frame_size, item.value.int_value);
        return true;
    case IEC_PARAMETER_VALUE_UINT:
        frame[frame_size++] = 4;
        write_uint_le(frame, frame_size, item.value.uint_value, 4);
        return true;
    case IEC_PARAMETER_VALUE_FLOAT:
        frame[frame_size++] = 4;
        write_float_le(frame, frame_size, item.value.float_value);
        return true;
    case IEC_PARAMETER_VALUE_ENUM:
        frame[frame_size++] = 4;
        write_uint_le(frame, frame_size, item.value.enum_value, 4);
        return true;
    case IEC_PARAMETER_VALUE_STRING: {
        const uint32_t length = static_cast<uint32_t>(std::strlen(item.value.string_value));
        if (length > kMaxParameterStringBytes || length > 255 || capacity < frame_size + 1 + length) {
            return false;
        }
        frame[frame_size++] = static_cast<uint8_t>(length);
        std::memcpy(frame + frame_size, item.value.string_value, length);
        frame_size += length;
        return true;
    }
    default:
        return false;
    }
}

bool decode_parameter_value(
    const uint8_t *payload,
    uint32_t payload_size,
    uint32_t &offset,
    iec_parameter_item_t &out,
    std::string *out_string) noexcept
{
    if (payload_size < offset + 11) {
        return false;
    }

    out = iec_parameter_item_t{};
    out.parameter_id = read_uint32_le(payload + offset, 4);
    offset += 4;
    out.address = read_uint32_le(payload + offset, 4);
    offset += 4;
    out.scope = static_cast<iec_parameter_scope_t>(payload[offset++]);
    out.value_type = static_cast<iec_parameter_value_type_t>(payload[offset++]);
    const uint8_t value_size = payload[offset++];

    if (!is_parameter_scope_valid(out.scope) || !is_parameter_value_type_valid(out.value_type) ||
        payload_size < offset + value_size) {
        return false;
    }

    switch (out.value_type) {
    case IEC_PARAMETER_VALUE_BOOL:
        if (value_size != 1 || payload[offset] > 1) {
            return false;
        }
        out.value.bool_value = payload[offset];
        break;
    case IEC_PARAMETER_VALUE_INT:
        if (value_size != 4) {
            return false;
        }
        out.value.int_value = read_int32_le(payload + offset);
        break;
    case IEC_PARAMETER_VALUE_UINT:
        if (value_size != 4) {
            return false;
        }
        out.value.uint_value = read_uint32_le(payload + offset, 4);
        break;
    case IEC_PARAMETER_VALUE_FLOAT:
        if (value_size != 4) {
            return false;
        }
        std::memcpy(&out.value.float_value, payload + offset, sizeof(float));
        break;
    case IEC_PARAMETER_VALUE_ENUM:
        if (value_size != 4) {
            return false;
        }
        out.value.enum_value = read_uint32_le(payload + offset, 4);
        break;
    case IEC_PARAMETER_VALUE_STRING:
        if (out_string == nullptr) {
            return false;
        }
        out_string->assign(reinterpret_cast<const char *>(payload + offset), value_size);
        out.value.string_value = out_string->c_str();
        break;
    default:
        return false;
    }

    offset += value_size;
    return true;
}

uint8_t parameter_type_id(iec_parameter_operation_t operation) noexcept
{
    switch (operation) {
    case IEC_PARAMETER_OPERATION_READ:
        return kAsduTypeParameterRead;
    case IEC_PARAMETER_OPERATION_WRITE:
        return kAsduTypeParameterWrite;
    case IEC_PARAMETER_OPERATION_VERIFY:
        return kAsduTypeParameterVerify;
    case IEC_PARAMETER_OPERATION_SWITCH_GROUP:
        return kAsduTypeSettingGroup;
    default:
        return 0;
    }
}

iec_parameter_operation_t parameter_operation_from_type_id(uint8_t type_id) noexcept
{
    switch (type_id) {
    case kAsduTypeParameterRead:
        return IEC_PARAMETER_OPERATION_READ;
    case kAsduTypeParameterWrite:
        return IEC_PARAMETER_OPERATION_WRITE;
    case kAsduTypeParameterVerify:
        return IEC_PARAMETER_OPERATION_VERIFY;
    case kAsduTypeSettingGroup:
        return IEC_PARAMETER_OPERATION_SWITCH_GROUP;
    default:
        return {};
    }
}

bool decode_command_result(
    const uint8_t *payload,
    uint32_t payload_size,
    const AsduLayout &layout,
    iec_command_result_t &out) noexcept
{
    if (payload == nullptr || payload_size < 2U + layout.cot_length + layout.common_address_length +
            layout.information_object_address_length) {
        return false;
    }

    iec_command_type_t command_type{};
    switch (payload[0]) {
    case kAsduTypeSingleCommand:
        command_type = IEC_COMMAND_SINGLE;
        break;
    case kAsduTypeDoubleCommand:
        command_type = IEC_COMMAND_DOUBLE;
        break;
    case kAsduTypeStepCommand:
        command_type = IEC_COMMAND_STEP;
        break;
    case kAsduTypeSetpointNormalized:
        command_type = IEC_COMMAND_SETPOINT_NORMALIZED;
        break;
    case kAsduTypeSetpointScaled:
        command_type = IEC_COMMAND_SETPOINT_SCALED;
        break;
    case kAsduTypeSetpointFloat:
        command_type = IEC_COMMAND_SETPOINT_FLOAT;
        break;
    default:
        return false;
    }

    const uint8_t variable_structure = payload[1];
    if ((variable_structure & 0x7FU) == 0) {
        return false;
    }

    const uint32_t cause_offset = 2U;
    const uint8_t cause = payload[cause_offset];
    const uint8_t originator = layout.cot_length == 2 ? payload[cause_offset + 1] : 0;
    const uint32_t common_address_offset = cause_offset + layout.cot_length;
    const uint16_t common_address =
        read_uint16_le(payload + common_address_offset, layout.common_address_length);
    const uint32_t info_offset = common_address_offset + layout.common_address_length;

    out = iec_command_result_t{};
    out.result = (cause & 0x40U) != 0 ? IEC_COMMAND_RESULT_NEGATIVE_CONFIRM : IEC_COMMAND_RESULT_ACCEPTED;
    out.is_final = 1;
    out.address.common_address = common_address;
    out.address.information_object_address =
        read_uint32_le(payload + info_offset, layout.information_object_address_length);
    out.address.type_id = payload[0];
    out.address.cause_of_transmission = static_cast<uint8_t>(cause & 0x3FU);
    out.address.originator_address = originator;

    (void)command_type;
    return true;
}

void dispatch_command_result(
    iec_session_t *session,
    const uint8_t *payload,
    uint32_t payload_size,
    const AsduLayout &layout,
    iec_on_command_result_fn callback,
    void *user_context) noexcept
{
    if (callback == nullptr) {
        return;
    }

    iec_command_result_t result{};
    if (!decode_command_result(payload, payload_size, layout, result)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        auto match = std::find_if(
            session->pending_commands.begin(),
            session->pending_commands.end(),
            [&result](const iec_session_t::PendingCommand &pending) {
                return pending.address.common_address == result.address.common_address &&
                    pending.address.information_object_address == result.address.information_object_address &&
                    pending.address.originator_address == result.address.originator_address;
            });
        if (match != session->pending_commands.end()) {
            result.command_id = match->command_id;
            result.semantic = match->semantic;
            session->pending_commands.erase(match);
        }
    }

    callback(session, &result, user_context);
}

void dispatch_clock_result(
    iec_session_t *session,
    const uint8_t *payload,
    uint32_t payload_size,
    const AsduLayout &layout,
    iec_on_clock_result_fn callback,
    void *user_context) noexcept
{
    if (callback == nullptr || payload == nullptr || payload_size < 2U + layout.cot_length +
            layout.common_address_length + layout.information_object_address_length) {
        return;
    }
    if (payload[0] != kAsduTypeClockSync || (payload[1] & 0x7FU) == 0) {
        return;
    }

    const uint32_t cause_offset = 2U;
    const uint8_t raw_cause = payload[cause_offset];
    const uint8_t cause = static_cast<uint8_t>(raw_cause & 0x3FU);
    const uint32_t common_address_offset = cause_offset + layout.cot_length;
    const uint16_t common_address =
        read_uint16_le(payload + common_address_offset, layout.common_address_length);
    const uint32_t info_offset = common_address_offset + layout.common_address_length;
    const uint32_t time_offset = info_offset + layout.information_object_address_length;

    iec_timestamp_t timestamp{};
    const bool timestamp_present = read_cp56_time2a(payload + time_offset, payload_size - time_offset, timestamp);

    iec_clock_result_t result{};
    result.common_address = common_address;
    result.result =
        (raw_cause & 0x40U) != 0 ? IEC_CLOCK_RESULT_NEGATIVE_CONFIRM : IEC_CLOCK_RESULT_ACCEPTED;
    result.cause_of_transmission = cause;

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        auto match = session->pending_clocks.end();
        if (cause == kCauseActivationConfirm) {
            match = std::find_if(
                session->pending_clocks.begin(),
                session->pending_clocks.end(),
                [common_address](const iec_session_t::PendingClock &pending) {
                    return pending.common_address == common_address &&
                        pending.operation == IEC_CLOCK_OPERATION_SYNC;
                });
        }
        if (match == session->pending_clocks.end()) {
            match = std::find_if(
                session->pending_clocks.begin(),
                session->pending_clocks.end(),
                [common_address, timestamp_present](const iec_session_t::PendingClock &pending) {
                    return pending.common_address == common_address &&
                        timestamp_present &&
                        pending.operation == IEC_CLOCK_OPERATION_READ;
                });
        }
        if (match == session->pending_clocks.end()) {
            return;
        }

        result.request_id = match->request_id;
        result.operation = match->operation;
        session->pending_clocks.erase(match);
    }

    if (result.operation == IEC_CLOCK_OPERATION_READ) {
        if (timestamp_present) {
            result.has_timestamp = 1;
            result.timestamp = timestamp;
        } else if (result.result == IEC_CLOCK_RESULT_ACCEPTED) {
            result.result = IEC_CLOCK_RESULT_PROTOCOL_ERROR;
        }
    }

    callback(session, &result, user_context);
}

void dispatch_parameter_messages(
    iec_session_t *session,
    const uint8_t *payload,
    uint32_t payload_size,
    const AsduLayout &layout,
    iec_on_parameter_indication_fn indication_callback,
    iec_on_parameter_result_fn result_callback,
    void *user_context) noexcept
{
    if (payload == nullptr || payload_size < 2U + layout.cot_length + layout.common_address_length +
            layout.information_object_address_length + 2U) {
        return;
    }

    const iec_parameter_operation_t operation = parameter_operation_from_type_id(payload[0]);
    if (operation == static_cast<iec_parameter_operation_t>(0) || (payload[1] & 0x7FU) == 0) {
        return;
    }

    const uint32_t cause_offset = 2U;
    const uint8_t raw_cause = payload[cause_offset];
    const uint8_t cause = static_cast<uint8_t>(raw_cause & 0x3FU);
    const uint32_t common_address_offset = cause_offset + layout.cot_length;
    const uint16_t common_address =
        read_uint16_le(payload + common_address_offset, layout.common_address_length);
    const uint32_t info_offset = common_address_offset + layout.common_address_length;
    uint32_t offset = info_offset + layout.information_object_address_length;

    const uint8_t setting_group = payload[offset++];
    const uint8_t flags = payload[offset++];
    const bool is_final = (flags & 0x01U) != 0 || cause == kCauseActivationTermination ||
        operation != IEC_PARAMETER_OPERATION_READ;
    const bool has_descriptor = (flags & 0x02U) != 0;

    uint32_t request_id = 0;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        auto match = std::find_if(
            session->pending_parameters.begin(),
            session->pending_parameters.end(),
            [operation, common_address, setting_group](const iec_session_t::PendingParameter &pending) {
                return pending.operation == operation && pending.common_address == common_address &&
                    (pending.setting_group == setting_group || pending.setting_group == 0 || setting_group == 0);
            });
        if (match == session->pending_parameters.end()) {
            return;
        }
        request_id = match->request_id;
        if (is_final) {
            session->pending_parameters.erase(match);
        }
    }

    if (operation == IEC_PARAMETER_OPERATION_READ && indication_callback != nullptr) {
        iec_parameter_indication_t indication{};
        indication.request_id = request_id;
        indication.operation = operation;
        indication.setting_group = setting_group;
        indication.is_final = is_final ? 1U : 0U;
        indication.has_descriptor = has_descriptor ? 1U : 0U;

        std::string string_value;
        if (payload_size > offset &&
            !decode_parameter_value(payload, payload_size, offset, indication.item, &string_value)) {
            return;
        }
        if (has_descriptor) {
            indication.descriptor.parameter_id = indication.item.parameter_id;
            indication.descriptor.address = indication.item.address;
            indication.descriptor.scope = indication.item.scope;
            indication.descriptor.value_type = indication.item.value_type;
            indication.descriptor.access = IEC_PARAMETER_ACCESS_READ_WRITE;
            indication.descriptor.supports_verify = 1;
        }

        indication_callback(session, &indication, user_context);
        return;
    }

    if (result_callback == nullptr) {
        return;
    }

    iec_parameter_result_t result{};
    result.request_id = request_id;
    result.operation = operation;
    result.setting_group = setting_group;
    result.is_final = is_final ? 1U : 0U;

    if (operation == IEC_PARAMETER_OPERATION_SWITCH_GROUP) {
        result.result = cause == kCauseRequest ? IEC_PARAMETER_RESULT_CURRENT_GROUP :
            ((raw_cause & 0x40U) != 0 ? IEC_PARAMETER_RESULT_REJECTED : IEC_PARAMETER_RESULT_GROUP_SWITCHED);
    } else if (operation == IEC_PARAMETER_OPERATION_VERIFY) {
        result.result =
            (raw_cause & 0x40U) != 0 ? IEC_PARAMETER_RESULT_VERIFY_MISMATCH : IEC_PARAMETER_RESULT_VERIFY_OK;
    } else {
        result.result = (raw_cause & 0x40U) != 0 ? IEC_PARAMETER_RESULT_REJECTED :
            IEC_PARAMETER_RESULT_ACCEPTED;
    }

    if (payload_size >= offset + 8) {
        result.parameter_id = read_uint32_le(payload + offset, 4);
        offset += 4;
        result.address = read_uint32_le(payload + offset, 4);
    }

    result_callback(session, &result, user_context);
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
        iec_on_command_result_fn command_callback = nullptr;
        iec_on_clock_result_fn clock_callback = nullptr;
        iec_on_parameter_indication_fn parameter_indication_callback = nullptr;
        iec_on_parameter_result_fn parameter_result_callback = nullptr;
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
            command_callback = session->callbacks.on_command_result;
            clock_callback = session->callbacks.on_clock_result;
            parameter_indication_callback = session->callbacks.on_parameter_indication;
            parameter_result_callback = session->callbacks.on_parameter_result;
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
        dispatch_command_result(
            session,
            buffer.data(),
            received,
            layout,
            command_callback,
            user_context);
        dispatch_clock_result(
            session,
            buffer.data(),
            received,
            layout,
            clock_callback,
            user_context);
        dispatch_parameter_messages(
            session,
            buffer.data(),
            received,
            layout,
            parameter_indication_callback,
            parameter_result_callback,
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

iec_status_t control_point(
    iec_session_t *session,
    const iec_command_request_t *request,
    uint32_t *out_command_id) noexcept
{
    if (out_command_id != nullptr) {
        *out_command_id = 0;
    }
    if (session == nullptr || request == nullptr || out_command_id == nullptr) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    if (!is_command_type_valid(request->command_type) ||
        !is_command_semantic_valid(request->semantic) ||
        !is_command_mode_valid(request->mode) ||
        !is_binary_flag(request->execute_on_ack) ||
        !is_command_value_valid(*request)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    if ((request->semantic == IEC_COMMAND_SEMANTIC_FACTORY_RESET ||
            request->semantic == IEC_COMMAND_SEMANTIC_DEVICE_REBOOT) &&
        request->command_type != IEC_COMMAND_SINGLE && request->command_type != IEC_COMMAND_DOUBLE) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    uint8_t frame[32]{};
    uint32_t frame_size = 0;
    uint32_t command_id = 0;
    uint8_t type_id = 0;
    iec_transport_t transport{};
    iec_on_raw_asdu_fn raw_callback = nullptr;
    void *user_context = nullptr;
    uint32_t timeout_ms = 0;
    bool raw_enabled = false;
    AsduLayout layout{};

    if (!command_type_id(request->command_type, type_id)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_RUNNING) {
            return IEC_STATUS_BAD_STATE;
        }

        layout = get_asdu_layout(*session);
        if (layout.cot_length == 0 || layout.common_address_length == 0 ||
            layout.information_object_address_length == 0 ||
            !fits_uint_le(request->address.common_address, layout.common_address_length) ||
            !fits_uint_le(request->address.information_object_address, layout.information_object_address_length)) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }

        frame[frame_size++] = type_id;
        frame[frame_size++] = 1;
        write_uint_le(frame, frame_size, kCauseActivation, 1);
        if (layout.cot_length == 2) {
            write_uint_le(frame, frame_size, request->address.originator_address, 1);
        }
        write_uint_le(frame, frame_size, request->address.common_address, layout.common_address_length);
        write_uint_le(
            frame,
            frame_size,
            request->address.information_object_address,
            layout.information_object_address_length);
        if (!append_command_value(*request, frame, frame_size)) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }

        if (frame_size > session->transport.max_plain_frame_len) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }

        command_id = session->next_command_id++;
        if (session->next_command_id == 0) {
            session->next_command_id = 1;
        }
        session->pending_commands.push_back(iec_session_t::PendingCommand{
            command_id,
            request->semantic,
            request->address,
        });
        transport = session->transport;
        raw_callback = session->callbacks.on_raw_asdu;
        user_context = session->config.user_context;
        timeout_ms = request->timeout_ms != 0 ? request->timeout_ms : session->config.command_timeout_ms;
        raw_enabled = session->config.enable_raw_asdu != 0;
    }

    const int send_result = transport.send(transport.ctx, frame, frame_size, timeout_ms);
    if (send_result != 0) {
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            auto match = std::find_if(
                session->pending_commands.begin(),
                session->pending_commands.end(),
                [command_id](const iec_session_t::PendingCommand &pending) {
                    return pending.command_id == command_id;
                });
            if (match != session->pending_commands.end()) {
                session->pending_commands.erase(match);
            }
        }
        return IEC_STATUS_IO_ERROR;
    }

    *out_command_id = command_id;

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

iec_status_t counter_interrogation(
    iec_session_t *session,
    const iec_counter_interrogation_request_t *request) noexcept
{
    if (session == nullptr || request == nullptr) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    if (request->qualifier < kCounterInterrogationMinQualifier ||
        request->qualifier > kCounterInterrogationMaxQualifier ||
        request->freeze > kCounterInterrogationMaxFreeze) {
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

        const uint8_t qualifier =
            static_cast<uint8_t>((request->freeze << 6U) | request->qualifier);
        frame[frame_size++] = kAsduTypeCounterInterrogation;
        frame[frame_size++] = 1;
        write_uint_le(frame, frame_size, kCauseActivation, 1);
        if (layout.cot_length == 2) {
            write_uint_le(frame, frame_size, kDefaultOriginatorAddress, 1);
        }
        write_uint_le(frame, frame_size, request->common_address, layout.common_address_length);
        write_uint_le(frame, frame_size, 0, layout.information_object_address_length);
        frame[frame_size++] = qualifier;

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

iec_status_t read_point(iec_session_t *session, const iec_point_address_t *address) noexcept
{
    if (session == nullptr || address == nullptr) {
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
            !fits_uint_le(address->common_address, layout.common_address_length) ||
            !fits_uint_le(address->information_object_address, layout.information_object_address_length)) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }

        frame[frame_size++] = kAsduTypeReadCommand;
        frame[frame_size++] = 1;
        write_uint_le(frame, frame_size, kCauseRequest, 1);
        if (layout.cot_length == 2) {
            write_uint_le(frame, frame_size, address->originator_address, 1);
        }
        write_uint_le(frame, frame_size, address->common_address, layout.common_address_length);
        write_uint_le(
            frame,
            frame_size,
            address->information_object_address,
            layout.information_object_address_length);

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

iec_status_t clock_sync(
    iec_session_t *session,
    const iec_clock_sync_request_t *request,
    uint32_t *out_request_id) noexcept
{
    if (out_request_id != nullptr) {
        *out_request_id = 0;
    }
    if (session == nullptr || request == nullptr || out_request_id == nullptr ||
        request->use_current_system_time > 1) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    uint8_t frame[24]{};
    uint32_t frame_size = 0;
    uint32_t request_id = 0;
    iec_transport_t transport{};
    iec_on_raw_asdu_fn raw_callback = nullptr;
    void *user_context = nullptr;
    uint32_t timeout_ms = 0;
    bool raw_enabled = false;
    AsduLayout layout{};
    const iec_timestamp_t timestamp =
        request->use_current_system_time != 0 ? current_system_timestamp() : request->timestamp;

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

        frame[frame_size++] = kAsduTypeClockSync;
        frame[frame_size++] = 1;
        write_uint_le(frame, frame_size, kCauseActivation, 1);
        if (layout.cot_length == 2) {
            write_uint_le(frame, frame_size, kDefaultOriginatorAddress, 1);
        }
        write_uint_le(frame, frame_size, request->common_address, layout.common_address_length);
        write_uint_le(
            frame,
            frame_size,
            kClockSyncInformationObjectAddress,
            layout.information_object_address_length);
        if (!write_cp56_time2a(frame, frame_size, timestamp)) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }

        if (frame_size > session->transport.max_plain_frame_len) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }

        request_id = take_next_request_id(*session);
        session->pending_clocks.push_back(iec_session_t::PendingClock{
            request_id,
            IEC_CLOCK_OPERATION_SYNC,
            request->common_address,
        });
        transport = session->transport;
        raw_callback = session->callbacks.on_raw_asdu;
        user_context = session->config.user_context;
        timeout_ms = session->config.command_timeout_ms;
        raw_enabled = session->config.enable_raw_asdu != 0;
    }

    const int send_result = transport.send(transport.ctx, frame, frame_size, timeout_ms);
    if (send_result != 0) {
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            auto match = std::find_if(
                session->pending_clocks.begin(),
                session->pending_clocks.end(),
                [request_id](const iec_session_t::PendingClock &pending) {
                    return pending.request_id == request_id;
                });
            if (match != session->pending_clocks.end()) {
                session->pending_clocks.erase(match);
            }
        }
        return IEC_STATUS_IO_ERROR;
    }

    *out_request_id = request_id;

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

iec_status_t read_clock(
    iec_session_t *session,
    const iec_clock_read_request_t *request,
    uint32_t *out_request_id) noexcept
{
    if (out_request_id != nullptr) {
        *out_request_id = 0;
    }
    if (session == nullptr || request == nullptr || out_request_id == nullptr) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    uint8_t frame[16]{};
    uint32_t frame_size = 0;
    uint32_t request_id = 0;
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

        frame[frame_size++] = kAsduTypeReadCommand;
        frame[frame_size++] = 1;
        write_uint_le(frame, frame_size, kCauseRequest, 1);
        if (layout.cot_length == 2) {
            write_uint_le(frame, frame_size, kDefaultOriginatorAddress, 1);
        }
        write_uint_le(frame, frame_size, request->common_address, layout.common_address_length);
        write_uint_le(
            frame,
            frame_size,
            kClockSyncInformationObjectAddress,
            layout.information_object_address_length);

        if (frame_size > session->transport.max_plain_frame_len) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }

        request_id = take_next_request_id(*session);
        session->pending_clocks.push_back(iec_session_t::PendingClock{
            request_id,
            IEC_CLOCK_OPERATION_READ,
            request->common_address,
        });
        transport = session->transport;
        raw_callback = session->callbacks.on_raw_asdu;
        user_context = session->config.user_context;
        timeout_ms = session->config.command_timeout_ms;
        raw_enabled = session->config.enable_raw_asdu != 0;
    }

    const int send_result = transport.send(transport.ctx, frame, frame_size, timeout_ms);
    if (send_result != 0) {
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            auto match = std::find_if(
                session->pending_clocks.begin(),
                session->pending_clocks.end(),
                [request_id](const iec_session_t::PendingClock &pending) {
                    return pending.request_id == request_id;
                });
            if (match != session->pending_clocks.end()) {
                session->pending_clocks.erase(match);
            }
        }
        return IEC_STATUS_IO_ERROR;
    }

    *out_request_id = request_id;

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

namespace {

iec_status_t send_parameter_request(
    iec_session_t *session,
    uint8_t *frame,
    uint32_t frame_size,
    uint16_t common_address,
    uint8_t setting_group,
    iec_parameter_operation_t operation,
    uint32_t *out_request_id) noexcept
{
    uint32_t request_id = 0;
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
        if (frame_size > session->transport.max_plain_frame_len) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }

        request_id = take_next_request_id(*session);
        session->pending_parameters.push_back(iec_session_t::PendingParameter{
            request_id,
            operation,
            common_address,
            setting_group,
        });
        transport = session->transport;
        raw_callback = session->callbacks.on_raw_asdu;
        user_context = session->config.user_context;
        timeout_ms = session->config.command_timeout_ms;
        raw_enabled = session->config.enable_raw_asdu != 0;
        layout = get_asdu_layout(*session);
    }

    const int send_result = transport.send(transport.ctx, frame, frame_size, timeout_ms);
    if (send_result != 0) {
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            auto match = std::find_if(
                session->pending_parameters.begin(),
                session->pending_parameters.end(),
                [request_id](const iec_session_t::PendingParameter &pending) {
                    return pending.request_id == request_id;
                });
            if (match != session->pending_parameters.end()) {
                session->pending_parameters.erase(match);
            }
        }
        return IEC_STATUS_IO_ERROR;
    }

    *out_request_id = request_id;

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

bool begin_parameter_frame(
    iec_session_t *session,
    iec_parameter_operation_t operation,
    uint16_t common_address,
    uint8_t setting_group,
    uint8_t flags,
    uint8_t *frame,
    uint32_t &frame_size,
    AsduLayout &layout) noexcept
{
    std::lock_guard<std::mutex> lock(session->mutex);
    layout = get_asdu_layout(*session);
    if (layout.cot_length == 0 || layout.common_address_length == 0 ||
        layout.information_object_address_length == 0 ||
        !fits_uint_le(common_address, layout.common_address_length)) {
        return false;
    }

    frame[frame_size++] = parameter_type_id(operation);
    frame[frame_size++] = 1;
    write_uint_le(
        frame,
        frame_size,
        operation == IEC_PARAMETER_OPERATION_READ ? kCauseRequest : kCauseActivation,
        1);
    if (layout.cot_length == 2) {
        write_uint_le(frame, frame_size, kDefaultOriginatorAddress, 1);
    }
    write_uint_le(frame, frame_size, common_address, layout.common_address_length);
    write_uint_le(
        frame,
        frame_size,
        kParameterChannelInformationObjectAddress,
        layout.information_object_address_length);
    frame[frame_size++] = setting_group;
    frame[frame_size++] = flags;
    return true;
}

} // namespace

iec_status_t read_parameters(
    iec_session_t *session,
    const iec_parameter_read_request_t *request,
    uint32_t *out_request_id) noexcept
{
    if (out_request_id != nullptr) {
        *out_request_id = 0;
    }
    if (session == nullptr || request == nullptr || out_request_id == nullptr ||
        !validate_parameter_read_request(*request)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    uint8_t frame[192]{};
    uint32_t frame_size = 0;
    AsduLayout layout{};
    const uint8_t flags = static_cast<uint8_t>(request->include_descriptor != 0 ? 0x02U : 0U);

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_RUNNING) {
            return IEC_STATUS_BAD_STATE;
        }
    }
    if (!begin_parameter_frame(
            session,
            IEC_PARAMETER_OPERATION_READ,
            request->common_address,
            request->setting_group,
            flags,
            frame,
            frame_size,
            layout)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    frame[frame_size++] = static_cast<uint8_t>(request->read_mode);
    frame[frame_size++] = static_cast<uint8_t>(request->scope);
    write_uint_le(frame, frame_size, request->start_address, 4);
    write_uint_le(frame, frame_size, request->end_address, 4);
    const uint32_t group_length =
        request->group_name == nullptr ? 0U : static_cast<uint32_t>(std::strlen(request->group_name));
    frame[frame_size++] = static_cast<uint8_t>(group_length);
    if (group_length > 0) {
        std::memcpy(frame + frame_size, request->group_name, group_length);
        frame_size += group_length;
    }

    return send_parameter_request(
        session,
        frame,
        frame_size,
        request->common_address,
        request->setting_group,
        IEC_PARAMETER_OPERATION_READ,
        out_request_id);
}

iec_status_t write_parameters(
    iec_session_t *session,
    const iec_parameter_write_request_t *request,
    uint32_t *out_request_id) noexcept
{
    if (out_request_id != nullptr) {
        *out_request_id = 0;
    }
    if (session == nullptr || request == nullptr || out_request_id == nullptr ||
        !is_binary_flag(request->verify_after_write) ||
        !validate_parameter_items(request->items, request->item_count)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    uint8_t frame[512]{};
    uint32_t frame_size = 0;
    AsduLayout layout{};
    const uint8_t flags = static_cast<uint8_t>(request->verify_after_write != 0 ? 0x02U : 0U);

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_RUNNING) {
            return IEC_STATUS_BAD_STATE;
        }
    }
    if (!begin_parameter_frame(
            session,
            IEC_PARAMETER_OPERATION_WRITE,
            request->common_address,
            request->setting_group,
            flags,
            frame,
            frame_size,
            layout)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    frame[frame_size++] = static_cast<uint8_t>(request->item_count);
    for (uint32_t i = 0; i < request->item_count; ++i) {
        if (!append_parameter_value(request->items[i], frame, frame_size, sizeof(frame))) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }
    }

    return send_parameter_request(
        session,
        frame,
        frame_size,
        request->common_address,
        request->setting_group,
        IEC_PARAMETER_OPERATION_WRITE,
        out_request_id);
}

iec_status_t verify_parameters(
    iec_session_t *session,
    const iec_parameter_verify_request_t *request,
    uint32_t *out_request_id) noexcept
{
    if (out_request_id != nullptr) {
        *out_request_id = 0;
    }
    if (session == nullptr || request == nullptr || out_request_id == nullptr ||
        !validate_parameter_items(request->expected_items, request->item_count)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    uint8_t frame[512]{};
    uint32_t frame_size = 0;
    AsduLayout layout{};

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_RUNNING) {
            return IEC_STATUS_BAD_STATE;
        }
    }
    if (!begin_parameter_frame(
            session,
            IEC_PARAMETER_OPERATION_VERIFY,
            request->common_address,
            request->setting_group,
            0,
            frame,
            frame_size,
            layout)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    frame[frame_size++] = static_cast<uint8_t>(request->item_count);
    for (uint32_t i = 0; i < request->item_count; ++i) {
        if (!append_parameter_value(request->expected_items[i], frame, frame_size, sizeof(frame))) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }
    }

    return send_parameter_request(
        session,
        frame,
        frame_size,
        request->common_address,
        request->setting_group,
        IEC_PARAMETER_OPERATION_VERIFY,
        out_request_id);
}

iec_status_t switch_setting_group(
    iec_session_t *session,
    const iec_setting_group_request_t *request,
    uint32_t *out_request_id) noexcept
{
    if (out_request_id != nullptr) {
        *out_request_id = 0;
    }
    if (session == nullptr || request == nullptr || out_request_id == nullptr ||
        !is_setting_group_action_valid(request->action) ||
        (request->action == IEC_SETTING_GROUP_ACTION_SWITCH && request->target_group == 0)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    uint8_t frame[32]{};
    uint32_t frame_size = 0;
    AsduLayout layout{};

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_RUNNING) {
            return IEC_STATUS_BAD_STATE;
        }
    }
    if (!begin_parameter_frame(
            session,
            IEC_PARAMETER_OPERATION_SWITCH_GROUP,
            request->common_address,
            request->target_group,
            0,
            frame,
            frame_size,
            layout)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    frame[frame_size++] = static_cast<uint8_t>(request->action);
    frame[frame_size++] = request->target_group;

    return send_parameter_request(
        session,
        frame,
        frame_size,
        request->common_address,
        request->target_group,
        IEC_PARAMETER_OPERATION_SWITCH_GROUP,
        out_request_id);
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
