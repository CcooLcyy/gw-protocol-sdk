#include "core/session.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
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
    struct DeferredRawAsduEvent {
        iec_raw_asdu_direction_t direction = IEC_RAW_ASDU_RX;
        std::vector<uint8_t> payload;
    };

    struct DeferredLinkEvent {
        iec_link_event_t event = IEC_LINK_EVENT_LINK_ERROR;
        iec_status_t reason = IEC_STATUS_INTERNAL_ERROR;
    };

    using DeferredAsyncEvent = std::variant<DeferredRawAsduEvent, DeferredLinkEvent>;

    struct PendingCommand {
        uint32_t command_id = 0;
        iec_command_semantic_t semantic = IEC_COMMAND_SEMANTIC_GENERAL;
        iec_point_address_t address{};
        uint8_t type_id = 0;
        uint8_t expected_confirm_cause = 0;
        std::chrono::steady_clock::time_point deadline{};
    };

    struct PendingClock {
        uint32_t request_id = 0;
        iec_clock_operation_t operation = IEC_CLOCK_OPERATION_SYNC;
        uint16_t common_address = 0;
        std::chrono::steady_clock::time_point deadline{};
    };

    struct PendingParameter {
        uint32_t request_id = 0;
        iec_parameter_operation_t operation = IEC_PARAMETER_OPERATION_READ;
        uint16_t common_address = 0;
        uint8_t setting_group = 0;
        iec_parameter_write_mode_t write_mode = IEC_PARAMETER_WRITE_MODE_NONE;
        std::chrono::steady_clock::time_point deadline{};
    };

    struct PendingFileList {
        uint32_t request_id = 0;
        uint16_t common_address = 0;
        std::string directory_name;
        std::chrono::steady_clock::time_point deadline{};
    };

    struct PendingUpgradeControl {
        uint32_t request_id = 0;
        uint16_t common_address = 0;
        uint32_t information_object_address = 0;
        iec_upgrade_operation_t operation = IEC_UPGRADE_OPERATION_START;
        std::chrono::steady_clock::time_point deadline{};
    };

    struct FileTransfer {
        uint32_t transfer_id = 0;
        iec_file_transfer_direction_t direction = IEC_FILE_TRANSFER_DIRECTION_READ;
        iec_file_transfer_state_t state = IEC_FILE_TRANSFER_STATE_ACCEPTED;
        uint16_t common_address = 0;
        std::string directory_name;
        std::string file_name;
        uint32_t total_size = 0;
        uint32_t acknowledged_offset = 0;
        uint8_t is_resumable = 1;
        iec_file_result_code_t last_result = IEC_FILE_RESULT_ACCEPTED;
        uint8_t last_cause_of_transmission = 0;
        int32_t last_native_error_code = 0;
        std::chrono::steady_clock::time_point deadline{};
    };

    gw::protocol::Profile profile;
    iec_session_config_t config;
    iec_transport_t transport;
    iec_callbacks_t callbacks;
    std::variant<m101_master_config_t, iec101_master_config_t, iec104_master_config_t> protocol_config;
    mutable std::mutex mutex;
    std::condition_variable lifecycle_cv;
    std::thread worker;
    std::vector<PendingCommand> pending_commands;
    std::vector<PendingClock> pending_clocks;
    std::vector<PendingParameter> pending_parameters;
    std::vector<PendingFileList> pending_file_lists;
    std::vector<PendingUpgradeControl> pending_upgrade_controls;
    std::vector<FileTransfer> file_transfers;
    std::vector<DeferredAsyncEvent> deferred_async_events;
    bool stop_requested = false;
    bool worker_finished = true;
    iec_runtime_state_t state = IEC_RUNTIME_CREATED;
    uint32_t next_command_id = 1;
    uint32_t next_request_id = 1;
    uint32_t next_transfer_id = 1;
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

bool begin_upgrade_control_frame(
    iec_session_t *session,
    iec_upgrade_operation_t operation,
    uint16_t common_address,
    uint32_t information_object_address,
    uint8_t *frame,
    uint32_t &frame_size,
    AsduLayout &layout) noexcept;

void notify_raw_asdu(
    iec_session_t *session,
    iec_raw_asdu_direction_t direction,
    const uint8_t *payload,
    uint32_t payload_size,
    const AsduLayout &layout,
    iec_on_raw_asdu_fn callback,
    void *user_context) noexcept;

iec_status_t send_file_frame(
    iec_session_t *session,
    const uint8_t *frame,
    uint32_t frame_size,
    uint32_t id,
    bool is_transfer,
    uint32_t *out_id) noexcept;

iec_status_t send_upgrade_control_frame(
    iec_session_t *session,
    const uint8_t *frame,
    uint32_t frame_size,
    uint32_t timeout_ms) noexcept;

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
constexpr uint8_t kAsduTypeSettingGroup = 205;
constexpr uint8_t kAsduTypeFileList = 206;
constexpr uint8_t kAsduTypeFileRead = 207;
constexpr uint8_t kAsduTypeFileWrite = 208;
constexpr uint8_t kAsduTypeUpgradeControl = 211;
constexpr uint8_t kCauseRequest = 5;
constexpr uint8_t kCauseActivation = 6;
constexpr uint8_t kCauseActivationConfirm = 7;
constexpr uint8_t kCauseDeactivation = 8;
constexpr uint8_t kCauseDeactivationConfirm = 9;
constexpr uint8_t kCauseActivationTermination = 10;
constexpr uint8_t kCauseUnknownTypeId = 44;
constexpr uint8_t kCauseUnknownInformationObjectAddress = 47;
constexpr uint8_t kDefaultOriginatorAddress = 0;
constexpr uint8_t kGeneralInterrogationMinQualifier = 20;
constexpr uint8_t kGeneralInterrogationMaxQualifier = 36;
constexpr uint8_t kCounterInterrogationMinQualifier = 1;
constexpr uint8_t kCounterInterrogationMaxQualifier = 5;
constexpr uint8_t kCounterInterrogationMaxFreeze = 3;
constexpr uint8_t kSelectExecuteQualifierMask = 0x80;
constexpr uint32_t kClockSyncInformationObjectAddress = 0;
constexpr uint32_t kParameterChannelInformationObjectAddress = 0;
constexpr uint32_t kFileChannelInformationObjectAddress = 0;
constexpr uint32_t kUpgradeChannelInformationObjectAddress = 0;
constexpr uint32_t kMaxParameterItemsPerRequest = 32;
constexpr uint32_t kMaxParameterStringBytes = 64;
constexpr uint32_t kMaxFileNameBytes = 128;
constexpr uint32_t kMaxFileChunkBytes = 1024;
constexpr uint32_t kIec101FileChunkBytes = 255;

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

bool is_parameter_write_mode_valid(iec_parameter_write_mode_t mode) noexcept
{
    switch (mode) {
    case IEC_PARAMETER_WRITE_MODE_PRESET:
    case IEC_PARAMETER_WRITE_MODE_EXECUTE:
    case IEC_PARAMETER_WRITE_MODE_CANCEL:
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
    if (!is_parameter_read_mode_valid(request.read_mode) || !is_parameter_scope_valid(request.scope)) {
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

bool validate_parameter_write_request(const iec_parameter_write_request_t &request) noexcept
{
    if (!is_parameter_write_mode_valid(request.mode) || request.reserved != 0) {
        return false;
    }
    if (request.mode == IEC_PARAMETER_WRITE_MODE_PRESET) {
        return validate_parameter_items(request.items, request.item_count);
    }
    return request.items == nullptr && request.item_count == 0;
}

bool is_upgrade_operation_valid(iec_upgrade_operation_t operation) noexcept
{
    switch (operation) {
    case IEC_UPGRADE_OPERATION_START:
    case IEC_UPGRADE_OPERATION_FINISH:
    case IEC_UPGRADE_OPERATION_CANCEL:
        return true;
    default:
        return false;
    }
}

uint8_t upgrade_control_cause(iec_upgrade_operation_t operation) noexcept
{
    return operation == IEC_UPGRADE_OPERATION_CANCEL ? kCauseDeactivation : kCauseActivation;
}

uint8_t upgrade_control_confirm_cause(iec_upgrade_operation_t operation) noexcept
{
    return operation == IEC_UPGRADE_OPERATION_CANCEL ? kCauseDeactivationConfirm : kCauseActivationConfirm;
}

uint8_t upgrade_control_se_bit(iec_upgrade_operation_t operation) noexcept
{
    return operation == IEC_UPGRADE_OPERATION_START ? 1U : 0U;
}

uint8_t command_cause(iec_command_mode_t mode) noexcept
{
    return mode == IEC_COMMAND_MODE_CANCEL ? kCauseDeactivation : kCauseActivation;
}

bool command_uses_select_bit(iec_command_mode_t mode) noexcept
{
    return mode == IEC_COMMAND_MODE_SELECT || mode == IEC_COMMAND_MODE_CANCEL;
}

uint8_t command_confirm_cause(iec_command_mode_t mode) noexcept
{
    return mode == IEC_COMMAND_MODE_CANCEL ? kCauseDeactivationConfirm : kCauseActivationConfirm;
}

bool is_dangerous_command_semantic(iec_command_semantic_t semantic) noexcept
{
    return semantic == IEC_COMMAND_SEMANTIC_FACTORY_RESET ||
        semantic == IEC_COMMAND_SEMANTIC_DEVICE_REBOOT;
}

bool same_command_protocol_key(
    const iec_session_t::PendingCommand &pending,
    const iec_point_address_t &address) noexcept
{
    return pending.address.common_address == address.common_address &&
        pending.address.information_object_address == address.information_object_address &&
        pending.address.originator_address == address.originator_address;
}

bool parameter_groups_overlap(uint8_t left, uint8_t right) noexcept
{
    return left == right || left == 0 || right == 0;
}

bool validate_raw_asdu_base(
    const uint8_t *payload,
    uint32_t payload_size,
    const AsduLayout &layout) noexcept
{
    if (payload == nullptr || layout.cot_length == 0 || layout.common_address_length == 0) {
        return false;
    }

    const uint32_t minimum_asdu_size = 2U + layout.cot_length + layout.common_address_length;
    if (payload_size < minimum_asdu_size) {
        return false;
    }

    const uint8_t type_id = payload[0];
    const uint8_t object_count = static_cast<uint8_t>(payload[1] & 0x7FU);
    const uint8_t cause = static_cast<uint8_t>(payload[2] & 0x3FU);
    return type_id != 0 && object_count != 0 && cause != 0;
}

bool is_non_empty_bounded_string(const char *value, uint32_t max_length) noexcept
{
    return value != nullptr && value[0] != '\0' && std::strlen(value) <= max_length;
}

bool append_string_field(
    uint8_t *frame,
    uint32_t &frame_size,
    uint32_t capacity,
    const char *value,
    uint32_t max_length) noexcept
{
    if (!is_non_empty_bounded_string(value, max_length)) {
        return false;
    }
    const uint32_t length = static_cast<uint32_t>(std::strlen(value));
    if (length > 255 || capacity < frame_size + 1 + length) {
        return false;
    }
    frame[frame_size++] = static_cast<uint8_t>(length);
    std::memcpy(frame + frame_size, value, length);
    frame_size += length;
    return true;
}

bool append_optional_string_field(
    uint8_t *frame,
    uint32_t &frame_size,
    uint32_t capacity,
    const char *value,
    uint32_t max_length) noexcept
{
    const uint32_t length = value == nullptr ? 0U : static_cast<uint32_t>(std::strlen(value));
    if (length > max_length || length > 255 || capacity < frame_size + 1 + length) {
        return false;
    }
    frame[frame_size++] = static_cast<uint8_t>(length);
    if (length > 0) {
        std::memcpy(frame + frame_size, value, length);
        frame_size += length;
    }
    return true;
}

bool read_string_field(
    const uint8_t *payload,
    uint32_t payload_size,
    uint32_t &offset,
    std::string &out,
    uint32_t max_length) noexcept
{
    if (payload_size < offset + 1) {
        return false;
    }
    const uint8_t length = payload[offset++];
    if (length == 0 || length > max_length || payload_size < offset + length) {
        return false;
    }
    out.assign(reinterpret_cast<const char *>(payload + offset), length);
    offset += length;
    return true;
}

uint32_t effective_file_chunk_size(const iec_session_t &session, uint32_t requested) noexcept
{
    uint32_t protocol_limit = kMaxFileChunkBytes;
    if (session.profile == Profile::M101) {
        const auto &config = std::get<m101_master_config_t>(session.protocol_config);
        protocol_limit = std::min<uint32_t>(protocol_limit, config.preferred_file_chunk_size);
    } else if (session.profile == Profile::IEC101) {
        protocol_limit = kIec101FileChunkBytes;
    }

    uint32_t chunk_size = requested == 0 ? protocol_limit : std::min<uint32_t>(requested, protocol_limit);
    return std::min<uint32_t>(chunk_size, session.transport.max_plain_frame_len);
}

uint32_t effective_file_read_chunk_size(
    const iec_session_t &session,
    uint32_t requested,
    const AsduLayout &layout) noexcept
{
    const uint32_t response_overhead =
        2U + layout.cot_length + layout.common_address_length + layout.information_object_address_length +
        2U + 4U + 16U;
    if (session.transport.max_plain_frame_len <= response_overhead) {
        return 0;
    }
    const uint32_t payload_limit = session.transport.max_plain_frame_len - response_overhead;
    return std::min<uint32_t>(effective_file_chunk_size(session, requested), payload_limit);
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

uint32_t take_next_transfer_id(iec_session_t &session) noexcept
{
    const uint32_t transfer_id = session.next_transfer_id++;
    if (session.next_transfer_id == 0) {
        session.next_transfer_id = 1;
    }
    return transfer_id;
}

std::chrono::steady_clock::time_point make_deadline(uint32_t timeout_ms) noexcept
{
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms == 0 ? 1U : timeout_ms);
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

void notify_link_event(iec_session_t *session, iec_link_event_t event, iec_status_t reason) noexcept
{
    iec_on_link_event_fn callback = nullptr;
    void *user_context = nullptr;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        callback = session->callbacks.on_link_event;
        user_context = session->config.user_context;
    }
    if (callback != nullptr) {
        callback(session, event, reason, user_context);
    }
}

void notify_log(iec_session_t *session, iec_log_level_t level, const char *message) noexcept
{
    iec_on_log_fn callback = nullptr;
    void *user_context = nullptr;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->config.enable_log_callback == 0 ||
            (session->config.initial_log_level != 0 && level > session->config.initial_log_level)) {
            return;
        }
        callback = session->callbacks.on_log;
        user_context = session->config.user_context;
    }
    if (callback != nullptr) {
        callback(session, level, message, user_context);
    }
}

void queue_raw_asdu_event(iec_session_t *session, iec_raw_asdu_direction_t direction, const uint8_t *payload, uint32_t payload_size) noexcept
{
    if (payload == nullptr || payload_size == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    try {
        session->deferred_async_events.emplace_back(iec_session_t::DeferredRawAsduEvent{
            direction,
            std::vector<uint8_t>(payload, payload + payload_size),
        });
    } catch (...) {
    }
}

void queue_link_event(iec_session_t *session, iec_link_event_t event, iec_status_t reason) noexcept
{
    std::lock_guard<std::mutex> lock(session->mutex);
    try {
        session->deferred_async_events.emplace_back(iec_session_t::DeferredLinkEvent{event, reason});
    } catch (...) {
    }
}

void dispatch_deferred_async_events(
    iec_session_t *session,
    const AsduLayout &layout,
    iec_on_raw_asdu_fn raw_callback,
    void *user_context) noexcept
{
    std::vector<iec_session_t::DeferredAsyncEvent> events;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        events.swap(session->deferred_async_events);
    }

    for (const auto &event : events) {
        if (std::holds_alternative<iec_session_t::DeferredRawAsduEvent>(event)) {
            const auto &raw = std::get<iec_session_t::DeferredRawAsduEvent>(event);
            notify_raw_asdu(
                session,
                raw.direction,
                raw.payload.data(),
                static_cast<uint32_t>(raw.payload.size()),
                layout,
                raw_callback,
                user_context);
            continue;
        }
        const auto &link = std::get<iec_session_t::DeferredLinkEvent>(event);
        notify_link_event(session, link.event, link.reason);
    }
}

iec_status_t change_state(iec_session_t *session, iec_runtime_state_t state) noexcept
{
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->state = state;
    }
    session->lifecycle_cv.notify_all();
    notify_state(session, state);
    return IEC_STATUS_OK;
}

void set_state_without_callback(iec_session_t *session, iec_runtime_state_t state) noexcept
{
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->state = state;
    }
    session->lifecycle_cv.notify_all();
}

void complete_worker_stop(iec_session_t *session) noexcept
{
    set_state_without_callback(session, IEC_RUNTIME_STOPPING);
    notify_state(session, IEC_RUNTIME_STOPPING);
    notify_link_event(session, IEC_LINK_EVENT_DISCONNECTED, IEC_STATUS_OK);
    notify_log(session, IEC_LOG_INFO, "protocol session stopped");
    set_state_without_callback(session, IEC_RUNTIME_STOPPED);
    notify_state(session, IEC_RUNTIME_STOPPED);
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->worker_finished = true;
    }
    session->lifecycle_cv.notify_all();
}

void dispatch_pending_timeouts(iec_session_t *session) noexcept
{
    const auto now = std::chrono::steady_clock::now();

    std::vector<iec_session_t::PendingCommand> timed_out_commands;
    std::vector<iec_session_t::PendingClock> timed_out_clocks;
    std::vector<iec_session_t::PendingParameter> timed_out_parameters;
    std::vector<iec_session_t::PendingFileList> timed_out_file_lists;
    std::vector<iec_session_t::FileTransfer> timed_out_transfers;
    std::vector<iec_session_t::PendingUpgradeControl> timed_out_upgrades;

    iec_on_command_result_fn command_callback = nullptr;
    iec_on_clock_result_fn clock_callback = nullptr;
    iec_on_parameter_result_fn parameter_callback = nullptr;
    iec_on_file_operation_result_fn file_callback = nullptr;
    iec_on_upgrade_result_fn upgrade_callback = nullptr;
    void *user_context = nullptr;

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_RUNNING) {
            return;
        }
        command_callback = session->callbacks.on_command_result;
        clock_callback = session->callbacks.on_clock_result;
        parameter_callback = session->callbacks.on_parameter_result;
        file_callback = session->callbacks.on_file_operation_result;
        upgrade_callback = session->callbacks.on_upgrade_result;
        user_context = session->config.user_context;

        auto command_out = std::remove_if(
            session->pending_commands.begin(),
            session->pending_commands.end(),
            [&](const iec_session_t::PendingCommand &pending) {
                if (pending.deadline <= now) {
                    timed_out_commands.push_back(pending);
                    return true;
                }
                return false;
            });
        session->pending_commands.erase(command_out, session->pending_commands.end());

        auto clock_out = std::remove_if(
            session->pending_clocks.begin(),
            session->pending_clocks.end(),
            [&](const iec_session_t::PendingClock &pending) {
                if (pending.deadline <= now) {
                    timed_out_clocks.push_back(pending);
                    return true;
                }
                return false;
            });
        session->pending_clocks.erase(clock_out, session->pending_clocks.end());

        auto parameter_out = std::remove_if(
            session->pending_parameters.begin(),
            session->pending_parameters.end(),
            [&](const iec_session_t::PendingParameter &pending) {
                if (pending.deadline <= now) {
                    timed_out_parameters.push_back(pending);
                    return true;
                }
                return false;
            });
        session->pending_parameters.erase(parameter_out, session->pending_parameters.end());

        auto file_list_out = std::remove_if(
            session->pending_file_lists.begin(),
            session->pending_file_lists.end(),
            [&](const iec_session_t::PendingFileList &pending) {
                if (pending.deadline <= now) {
                    timed_out_file_lists.push_back(pending);
                    return true;
                }
                return false;
            });
        session->pending_file_lists.erase(file_list_out, session->pending_file_lists.end());

        for (auto &transfer : session->file_transfers) {
            if (transfer.deadline <= now &&
                transfer.state != IEC_FILE_TRANSFER_STATE_COMPLETED &&
                transfer.state != IEC_FILE_TRANSFER_STATE_FAILED) {
                transfer.state = IEC_FILE_TRANSFER_STATE_FAILED;
                transfer.last_result = IEC_FILE_RESULT_TIMEOUT;
                timed_out_transfers.push_back(transfer);
            }
        }

        auto upgrade_out = std::remove_if(
            session->pending_upgrade_controls.begin(),
            session->pending_upgrade_controls.end(),
            [&](const iec_session_t::PendingUpgradeControl &pending) {
                if (pending.deadline <= now) {
                    timed_out_upgrades.push_back(pending);
                    return true;
                }
                return false;
            });
        session->pending_upgrade_controls.erase(upgrade_out, session->pending_upgrade_controls.end());
    }

    for (const auto &pending : timed_out_commands) {
        if (command_callback == nullptr) {
            continue;
        }
        iec_command_result_t result{};
        result.command_id = pending.command_id;
        result.semantic = pending.semantic;
        result.result = IEC_COMMAND_RESULT_TIMEOUT;
        result.address = pending.address;
        result.is_final = 1;
        command_callback(session, &result, user_context);
    }

    for (const auto &pending : timed_out_clocks) {
        if (clock_callback == nullptr) {
            continue;
        }
        iec_clock_result_t result{};
        result.request_id = pending.request_id;
        result.operation = pending.operation;
        result.result = IEC_CLOCK_RESULT_TIMEOUT;
        result.common_address = pending.common_address;
        clock_callback(session, &result, user_context);
    }

    for (const auto &pending : timed_out_parameters) {
        if (parameter_callback == nullptr) {
            continue;
        }
        iec_parameter_result_t result{};
        result.request_id = pending.request_id;
        result.operation = pending.operation;
        result.result = IEC_PARAMETER_RESULT_TIMEOUT;
        result.setting_group = pending.setting_group;
        result.write_mode = pending.write_mode;
        result.is_final = 1;
        parameter_callback(session, &result, user_context);
    }

    for (const auto &pending : timed_out_file_lists) {
        if (file_callback == nullptr) {
            continue;
        }
        iec_file_operation_result_t result{};
        result.request_id = pending.request_id;
        result.operation = IEC_FILE_OPERATION_LIST;
        result.result = IEC_FILE_RESULT_TIMEOUT;
        result.common_address = pending.common_address;
        result.directory_name = pending.directory_name.c_str();
        result.detail_message = "file list timeout";
        result.is_final = 1;
        file_callback(session, &result, user_context);
    }

    for (const auto &transfer : timed_out_transfers) {
        if (file_callback == nullptr) {
            continue;
        }
        iec_file_operation_result_t result{};
        result.transfer_id = transfer.transfer_id;
        result.operation = transfer.direction == IEC_FILE_TRANSFER_DIRECTION_READ
            ? IEC_FILE_OPERATION_READ
            : IEC_FILE_OPERATION_WRITE;
        result.direction = transfer.direction;
        result.result = IEC_FILE_RESULT_TIMEOUT;
        result.common_address = transfer.common_address;
        result.directory_name = transfer.directory_name.c_str();
        result.file_name = transfer.file_name.c_str();
        result.final_offset = transfer.acknowledged_offset;
        result.total_size = transfer.total_size;
        result.detail_message = "file transfer timeout";
        result.is_final = 1;
        file_callback(session, &result, user_context);
    }

    for (const auto &pending : timed_out_upgrades) {
        if (upgrade_callback == nullptr) {
            continue;
        }
        iec_upgrade_result_t result{};
        result.request_id = pending.request_id;
        result.common_address = pending.common_address;
        result.information_object_address = pending.information_object_address;
        result.operation = pending.operation;
        result.result = IEC_UPGRADE_RESULT_TIMEOUT;
        result.detail_message = "upgrade control timeout";
        result.is_final = 1;
        upgrade_callback(session, &result, user_context);
    }
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
            (command_uses_select_bit(request.mode) ? kSelectExecuteQualifierMask : 0U) |
            ((request.qualifier & 0x1FU) << 2U));
        return true;
    case IEC_COMMAND_DOUBLE:
        frame[frame_size++] = static_cast<uint8_t>(
            (request.value.doubled & 0x03U) |
            (command_uses_select_bit(request.mode) ? kSelectExecuteQualifierMask : 0U) |
            ((request.qualifier & 0x1FU) << 2U));
        return true;
    case IEC_COMMAND_STEP:
        frame[frame_size++] = static_cast<uint8_t>(request.value.step);
        frame[frame_size++] = static_cast<uint8_t>(
            (command_uses_select_bit(request.mode) ? kSelectExecuteQualifierMask : 0U) |
            (request.qualifier & 0x1FU));
        return true;
    case IEC_COMMAND_SETPOINT_NORMALIZED:
        write_int16_le(frame, frame_size, request.value.normalized);
        frame[frame_size++] = static_cast<uint8_t>(
            (command_uses_select_bit(request.mode) ? kSelectExecuteQualifierMask : 0U) |
            (request.qualifier & 0x1FU));
        return true;
    case IEC_COMMAND_SETPOINT_SCALED:
        write_int16_le(frame, frame_size, request.value.scaled);
        frame[frame_size++] = static_cast<uint8_t>(
            (command_uses_select_bit(request.mode) ? kSelectExecuteQualifierMask : 0U) |
            (request.qualifier & 0x1FU));
        return true;
    case IEC_COMMAND_SETPOINT_FLOAT:
        write_float_le(frame, frame_size, request.value.short_float);
        frame[frame_size++] = static_cast<uint8_t>(
            (command_uses_select_bit(request.mode) ? kSelectExecuteQualifierMask : 0U) |
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
    case kAsduTypeSettingGroup:
        return IEC_PARAMETER_OPERATION_SWITCH_GROUP;
    default:
        return {};
    }
}

uint8_t file_type_id(iec_file_operation_t operation) noexcept
{
    switch (operation) {
    case IEC_FILE_OPERATION_LIST:
        return kAsduTypeFileList;
    case IEC_FILE_OPERATION_READ:
        return kAsduTypeFileRead;
    case IEC_FILE_OPERATION_WRITE:
        return kAsduTypeFileWrite;
    default:
        return 0;
    }
}

iec_file_operation_t file_operation_from_type_id(uint8_t type_id) noexcept
{
    switch (type_id) {
    case kAsduTypeFileList:
        return IEC_FILE_OPERATION_LIST;
    case kAsduTypeFileRead:
        return IEC_FILE_OPERATION_READ;
    case kAsduTypeFileWrite:
        return IEC_FILE_OPERATION_WRITE;
    default:
        return {};
    }
}

iec_file_result_code_t file_result_from_cause(uint8_t raw_cause, uint8_t result_hint) noexcept
{
    if (result_hint > IEC_FILE_RESULT_ACCEPTED && result_hint <= IEC_FILE_RESULT_UNSUPPORTED) {
        return static_cast<iec_file_result_code_t>(result_hint);
    }
    if ((raw_cause & 0x40U) != 0) {
        return IEC_FILE_RESULT_NEGATIVE_CONFIRM;
    }
    const uint8_t cause = static_cast<uint8_t>(raw_cause & 0x3FU);
    if (cause == kCauseActivationTermination) {
        return IEC_FILE_RESULT_COMPLETED;
    }
    if (cause != kCauseActivationConfirm) {
        return IEC_FILE_RESULT_PROTOCOL_ERROR;
    }
    return IEC_FILE_RESULT_ACCEPTED;
}

const char *file_detail_message(iec_file_result_code_t result) noexcept
{
    switch (result) {
    case IEC_FILE_RESULT_REJECTED:
        return "file operation rejected";
    case IEC_FILE_RESULT_NEGATIVE_CONFIRM:
        return "file operation negative confirmation";
    case IEC_FILE_RESULT_OFFSET_MISMATCH:
        return "file offset mismatch";
    case IEC_FILE_RESULT_PROTOCOL_ERROR:
        return "unexpected file cause";
    case IEC_FILE_RESULT_NOT_FOUND:
        return "file not found";
    case IEC_FILE_RESULT_UNSUPPORTED:
        return "file operation unsupported";
    default:
        return nullptr;
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
                return same_command_protocol_key(pending, result.address);
            });
        if (match != session->pending_commands.end()) {
            result.command_id = match->command_id;
            result.semantic = match->semantic;
            if (match->type_id != result.address.type_id ||
                match->expected_confirm_cause != result.address.cause_of_transmission) {
                result.result = IEC_COMMAND_RESULT_PROTOCOL_ERROR;
            }
            session->pending_commands.erase(match);
        } else {
            return;
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
    const uint8_t type_id = payload[0];
    if ((type_id != kAsduTypeClockSync && type_id != kAsduTypeReadCommand) || (payload[1] & 0x7FU) == 0) {
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
    const bool timestamp_present = type_id == kAsduTypeClockSync &&
        read_cp56_time2a(payload + time_offset, payload_size - time_offset, timestamp);

    iec_clock_result_t result{};
    result.common_address = common_address;
    result.cause_of_transmission = cause;

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        auto match = session->pending_clocks.end();
        if (type_id == kAsduTypeClockSync && cause == kCauseActivationConfirm) {
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
                [common_address](const iec_session_t::PendingClock &pending) {
                    return pending.common_address == common_address &&
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
        if ((raw_cause & 0x40U) != 0) {
            result.result = IEC_CLOCK_RESULT_NEGATIVE_CONFIRM;
        } else if (cause == kCauseUnknownTypeId || cause == kCauseUnknownInformationObjectAddress) {
            result.result = IEC_CLOCK_RESULT_UNSUPPORTED;
        } else if (type_id != kAsduTypeClockSync || cause != kCauseRequest) {
            result.result = IEC_CLOCK_RESULT_PROTOCOL_ERROR;
        } else if (timestamp_present) {
            result.result = IEC_CLOCK_RESULT_ACCEPTED;
            result.has_timestamp = 1;
            result.timestamp = timestamp;
        } else {
            result.result = IEC_CLOCK_RESULT_PROTOCOL_ERROR;
        }
    } else if ((raw_cause & 0x40U) != 0) {
        result.result = IEC_CLOCK_RESULT_NEGATIVE_CONFIRM;
    } else if (type_id != kAsduTypeClockSync || cause != kCauseActivationConfirm) {
        result.result = IEC_CLOCK_RESULT_PROTOCOL_ERROR;
    } else {
        result.result = IEC_CLOCK_RESULT_ACCEPTED;
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
    const auto write_mode = static_cast<iec_parameter_write_mode_t>((flags >> 2U) & 0x03U);

    uint32_t request_id = 0;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        auto match = std::find_if(
            session->pending_parameters.begin(),
            session->pending_parameters.end(),
            [operation, common_address, setting_group](const iec_session_t::PendingParameter &pending) {
                return pending.operation == operation && pending.common_address == common_address &&
                    parameter_groups_overlap(pending.setting_group, setting_group);
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

        std::string string_value;
        if (payload_size > offset &&
            !decode_parameter_value(payload, payload_size, offset, indication.item, &string_value)) {
            return;
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
    result.write_mode =
        operation == IEC_PARAMETER_OPERATION_WRITE && is_parameter_write_mode_valid(write_mode)
        ? write_mode
        : IEC_PARAMETER_WRITE_MODE_NONE;
    result.is_final = is_final ? 1U : 0U;

    if (operation == IEC_PARAMETER_OPERATION_SWITCH_GROUP) {
        result.result = cause == kCauseRequest ? IEC_PARAMETER_RESULT_CURRENT_GROUP :
            ((raw_cause & 0x40U) != 0 ? IEC_PARAMETER_RESULT_REJECTED : IEC_PARAMETER_RESULT_GROUP_SWITCHED);
    } else if ((raw_cause & 0x40U) != 0) {
        result.result = IEC_PARAMETER_RESULT_REJECTED;
    } else if (result.write_mode == IEC_PARAMETER_WRITE_MODE_PRESET) {
        result.result = IEC_PARAMETER_RESULT_PRESET_OK;
    } else if (result.write_mode == IEC_PARAMETER_WRITE_MODE_EXECUTE) {
        result.result = IEC_PARAMETER_RESULT_EXECUTE_OK;
    } else if (result.write_mode == IEC_PARAMETER_WRITE_MODE_CANCEL) {
        result.result = IEC_PARAMETER_RESULT_CANCEL_OK;
    } else {
        result.result = IEC_PARAMETER_RESULT_ACCEPTED;
    }

    if (payload_size >= offset + 8) {
        result.parameter_id = read_uint32_le(payload + offset, 4);
        offset += 4;
        result.address = read_uint32_le(payload + offset, 4);
    }

    result_callback(session, &result, user_context);
}

void dispatch_file_messages(
    iec_session_t *session,
    const uint8_t *payload,
    uint32_t payload_size,
    const AsduLayout &layout,
    iec_on_file_list_indication_fn list_callback,
    iec_on_file_data_indication_fn data_callback,
    iec_on_file_operation_result_fn result_callback,
    void *user_context) noexcept
{
    if (payload == nullptr || payload_size < 2U + layout.cot_length + layout.common_address_length +
            layout.information_object_address_length + 2U) {
        return;
    }

    const iec_file_operation_t operation = file_operation_from_type_id(payload[0]);
    if (operation == static_cast<iec_file_operation_t>(0) || (payload[1] & 0x7FU) == 0) {
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

    const uint8_t flags = payload[offset++];
    const uint8_t result_hint = payload[offset++];
    const bool is_final = (flags & 0x01U) != 0 || cause == kCauseActivationTermination;
    const iec_file_result_code_t result_code = file_result_from_cause(raw_cause, result_hint);
    const bool file_failure = result_code != IEC_FILE_RESULT_ACCEPTED &&
        result_code != IEC_FILE_RESULT_COMPLETED;

    if (operation == IEC_FILE_OPERATION_LIST) {
        std::string directory;
        if (!read_string_field(payload, payload_size, offset, directory, kMaxFileNameBytes)) {
            return;
        }

        uint32_t request_id = 0;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            auto match = std::find_if(
                session->pending_file_lists.begin(),
                session->pending_file_lists.end(),
                [common_address, &directory](const iec_session_t::PendingFileList &pending) {
                    return pending.common_address == common_address && pending.directory_name == directory;
                });
            if (match == session->pending_file_lists.end()) {
                return;
            }
            request_id = match->request_id;
            if (is_final || file_failure) {
                session->pending_file_lists.erase(match);
            }
        }

        if (list_callback != nullptr && !file_failure) {
            std::vector<iec_file_entry_t> entries;
            std::vector<std::string> directories;
            std::vector<std::string> file_names;
            std::vector<std::string> checksums;
            if (payload_size > offset) {
                const uint8_t entry_count = payload[offset++];
                try {
                    entries.resize(entry_count);
                    directories.resize(entry_count);
                    file_names.resize(entry_count);
                    checksums.resize(entry_count);
                } catch (...) {
                    return;
                }
                for (uint8_t i = 0; i < entry_count; ++i) {
                    std::string entry_directory;
                    std::string file_name;
                    std::string checksum;
                    if (!read_string_field(payload, payload_size, offset, entry_directory, kMaxFileNameBytes) ||
                        !read_string_field(payload, payload_size, offset, file_name, kMaxFileNameBytes) ||
                        payload_size < offset + 15) {
                        return;
                    }
                    directories[i] = std::move(entry_directory);
                    file_names[i] = std::move(file_name);
                    entries[i].directory_name = directories[i].c_str();
                    entries[i].file_name = file_names[i].c_str();
                    entries[i].file_size = read_uint32_le(payload + offset, 4);
                    offset += 4;
                    const uint32_t timestamp_low = read_uint32_le(payload + offset, 4);
                    offset += 4;
                    const uint32_t timestamp_high = read_uint32_le(payload + offset, 4);
                    offset += 4;
                    entries[i].modified_timestamp_ms =
                        (static_cast<uint64_t>(timestamp_high) << 32U) | timestamp_low;
                    entries[i].is_directory = payload[offset++];
                    entries[i].is_read_only = payload[offset++];
                    const uint8_t checksum_length = payload[offset++];
                    if (payload_size < offset + checksum_length) {
                        return;
                    }
                    checksums[i].assign(reinterpret_cast<const char *>(payload + offset), checksum_length);
                    offset += checksum_length;
                    entries[i].checksum_text = checksums[i].empty() ? nullptr : checksums[i].c_str();
                }
            }

            iec_file_list_indication_t indication{};
            indication.request_id = request_id;
            indication.common_address = common_address;
            indication.directory_name = directory.c_str();
            indication.entries = entries.empty() ? nullptr : entries.data();
            indication.entry_count = static_cast<uint32_t>(entries.size());
            indication.is_final = is_final ? 1U : 0U;
            list_callback(session, &indication, user_context);
        }

        if ((is_final || file_failure) && result_callback != nullptr) {
            iec_file_operation_result_t result{};
            result.request_id = request_id;
            result.operation = IEC_FILE_OPERATION_LIST;
            result.result = result_code == IEC_FILE_RESULT_ACCEPTED ? IEC_FILE_RESULT_COMPLETED : result_code;
            result.common_address = common_address;
            result.directory_name = directory.c_str();
            result.cause_of_transmission = cause;
            result.detail_message = file_detail_message(result.result);
            result.is_final = 1;
            result_callback(session, &result, user_context);
        }
        return;
    }

    if (payload_size < offset + 4) {
        return;
    }
    const uint32_t transfer_id = read_uint32_le(payload + offset, 4);
    offset += 4;

    iec_session_t::FileTransfer transfer_snapshot{};
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        auto match = std::find_if(
            session->file_transfers.begin(),
            session->file_transfers.end(),
            [transfer_id](const iec_session_t::FileTransfer &transfer) {
                return transfer.transfer_id == transfer_id;
            });
        if (match == session->file_transfers.end()) {
            return;
        }
        match->last_result = result_code;
        match->last_cause_of_transmission = cause;
        if (result_code == IEC_FILE_RESULT_COMPLETED) {
            match->state = IEC_FILE_TRANSFER_STATE_COMPLETED;
            match->is_resumable = 0;
        } else if (result_code == IEC_FILE_RESULT_ACCEPTED) {
            match->state = IEC_FILE_TRANSFER_STATE_RUNNING;
        } else {
            match->state = IEC_FILE_TRANSFER_STATE_FAILED;
            match->is_resumable = 0;
        }
        transfer_snapshot = *match;
    }

    if (operation == IEC_FILE_OPERATION_READ) {
        if (payload_size < offset + 16) {
            if (!file_failure) {
                return;
            }
        } else {
            const uint32_t current_offset = read_uint32_le(payload + offset, 4);
            offset += 4;
            const uint32_t next_offset = read_uint32_le(payload + offset, 4);
            offset += 4;
            const uint32_t total_size = read_uint32_le(payload + offset, 4);
            offset += 4;
            const uint32_t data_size = read_uint32_le(payload + offset, 4);
            offset += 4;
            if (payload_size < offset + data_size) {
                return;
            }

            {
                std::lock_guard<std::mutex> lock(session->mutex);
                auto match = std::find_if(
                    session->file_transfers.begin(),
                    session->file_transfers.end(),
                    [transfer_id](const iec_session_t::FileTransfer &transfer) {
                        return transfer.transfer_id == transfer_id;
                    });
                if (match != session->file_transfers.end()) {
                    match->acknowledged_offset = next_offset;
                    if (total_size != 0) {
                        match->total_size = total_size;
                    }
                    match->deadline = make_deadline(session->config.command_timeout_ms);
                    transfer_snapshot = *match;
                }
            }

            if (data_callback != nullptr && !file_failure) {
                iec_file_data_indication_t indication{};
                indication.transfer_id = transfer_id;
                indication.direction = IEC_FILE_TRANSFER_DIRECTION_READ;
                indication.common_address = common_address;
                indication.directory_name = transfer_snapshot.directory_name.c_str();
                indication.file_name = transfer_snapshot.file_name.c_str();
                indication.total_size = transfer_snapshot.total_size;
                indication.current_offset = current_offset;
                indication.next_offset = next_offset;
                indication.data = payload + offset;
                indication.data_size = data_size;
                indication.is_final = is_final ? 1U : 0U;
                data_callback(session, &indication, user_context);
            }
        }
    }

    if (operation == IEC_FILE_OPERATION_WRITE && payload_size >= offset + 8) {
        const uint32_t acknowledged_offset = read_uint32_le(payload + offset, 4);
        offset += 4;
        const uint32_t total_size = read_uint32_le(payload + offset, 4);
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            auto match = std::find_if(
                session->file_transfers.begin(),
                session->file_transfers.end(),
                [transfer_id](const iec_session_t::FileTransfer &transfer) {
                    return transfer.transfer_id == transfer_id;
                });
            if (match != session->file_transfers.end()) {
                match->acknowledged_offset = acknowledged_offset;
                if (total_size != 0) {
                    match->total_size = total_size;
                }
                match->deadline = make_deadline(session->config.command_timeout_ms);
                transfer_snapshot = *match;
            }
        }
    }
    if ((is_final || file_failure || operation == IEC_FILE_OPERATION_WRITE) && result_callback != nullptr) {
        iec_file_operation_result_t result{};
        result.transfer_id = transfer_id;
        result.operation = operation;
        result.direction = transfer_snapshot.direction;
        result.result = result_code;
        result.common_address = common_address;
        result.directory_name = transfer_snapshot.directory_name.c_str();
        result.file_name = transfer_snapshot.file_name.c_str();
        result.final_offset = transfer_snapshot.acknowledged_offset;
        result.total_size = transfer_snapshot.total_size;
        result.cause_of_transmission = cause;
        result.detail_message = file_detail_message(result.result);
        result.is_final = (is_final || file_failure) ? 1U : 0U;
        result_callback(session, &result, user_context);
    }
}

void dispatch_upgrade_control_messages(
    iec_session_t *session,
    const uint8_t *payload,
    uint32_t payload_size,
    const AsduLayout &layout) noexcept
{
    if (payload == nullptr || payload_size < 2U + layout.cot_length + layout.common_address_length +
            layout.information_object_address_length + 1U) {
        return;
    }
    if (payload[0] != kAsduTypeUpgradeControl || (payload[1] & 0x7FU) == 0) {
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
    const uint32_t information_object_address =
        read_uint32_le(payload + info_offset, layout.information_object_address_length);

    const uint8_t control_type = payload[offset++];

    iec_session_t::PendingUpgradeControl pending{};
    bool protocol_error = false;
    iec_on_upgrade_result_fn callback = nullptr;
    void *user_context = nullptr;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        auto match = std::find_if(
            session->pending_upgrade_controls.begin(),
            session->pending_upgrade_controls.end(),
            [common_address, information_object_address](const iec_session_t::PendingUpgradeControl &item) {
                const uint32_t expected_information_object_address = item.information_object_address == 0
                    ? kUpgradeChannelInformationObjectAddress
                    : item.information_object_address;
                return item.common_address == common_address &&
                    expected_information_object_address == information_object_address;
            });
        if (match == session->pending_upgrade_controls.end()) {
            return;
        }
        pending = *match;
        protocol_error = upgrade_control_confirm_cause(pending.operation) != cause ||
            upgrade_control_se_bit(pending.operation) != control_type;
        session->pending_upgrade_controls.erase(match);
        callback = session->callbacks.on_upgrade_result;
        user_context = session->config.user_context;
    }

    if (callback == nullptr) {
        return;
    }

    iec_upgrade_result_t result{};
    result.request_id = pending.request_id;
    result.common_address = common_address;
    result.information_object_address = information_object_address == 0
        ? pending.information_object_address
        : information_object_address;
    result.operation = pending.operation;
    result.cause_of_transmission = cause;
    result.is_final = 1;
    if (protocol_error) {
        result.result = IEC_UPGRADE_RESULT_PROTOCOL_ERROR;
        result.detail_message = "unexpected upgrade control cause";
    } else if ((raw_cause & 0x40U) != 0) {
        result.result = IEC_UPGRADE_RESULT_NEGATIVE_CONFIRM;
        result.detail_message = "upgrade control negative confirmation";
    } else if (result.operation == IEC_UPGRADE_OPERATION_CANCEL) {
        result.result = IEC_UPGRADE_RESULT_CANCELED;
    } else {
        result.result = IEC_UPGRADE_RESULT_ACCEPTED;
    }
    callback(session, &result, user_context);
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
    notify_state(session, IEC_RUNTIME_STARTING);
    notify_link_event(session, IEC_LINK_EVENT_CONNECTING, IEC_STATUS_OK);
    notify_log(session, IEC_LOG_INFO, "protocol session starting");
    set_state_without_callback(session, IEC_RUNTIME_RUNNING);
    notify_state(session, IEC_RUNTIME_RUNNING);
    notify_link_event(session, IEC_LINK_EVENT_CONNECTED, IEC_STATUS_OK);
    notify_log(session, IEC_LOG_INFO, "protocol session running");

    for (;;) {
        iec_transport_t transport{};
        iec_on_raw_asdu_fn raw_callback = nullptr;
        iec_on_point_indication_fn point_callback = nullptr;
        iec_on_command_result_fn command_callback = nullptr;
        iec_on_clock_result_fn clock_callback = nullptr;
        iec_on_parameter_indication_fn parameter_indication_callback = nullptr;
        iec_on_parameter_result_fn parameter_result_callback = nullptr;
        iec_on_file_list_indication_fn file_list_callback = nullptr;
        iec_on_file_data_indication_fn file_data_callback = nullptr;
        iec_on_file_operation_result_fn file_result_callback = nullptr;
        void *user_context = nullptr;
        uint32_t recv_timeout_ms = 50;
        uint32_t max_frame_len = 0;
        bool raw_enabled = false;
        AsduLayout layout{};

        bool should_stop = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->stop_requested || session->state != IEC_RUNTIME_RUNNING) {
                should_stop = true;
            } else {
                transport = session->transport;
                raw_callback = session->callbacks.on_raw_asdu;
                point_callback = session->callbacks.on_point_indication;
                command_callback = session->callbacks.on_command_result;
                clock_callback = session->callbacks.on_clock_result;
                parameter_indication_callback = session->callbacks.on_parameter_indication;
                parameter_result_callback = session->callbacks.on_parameter_result;
                file_list_callback = session->callbacks.on_file_list_indication;
                file_data_callback = session->callbacks.on_file_data_indication;
                file_result_callback = session->callbacks.on_file_operation_result;
                user_context = session->config.user_context;
                recv_timeout_ms = std::min<uint32_t>(session->config.command_timeout_ms, 50U);
                if (recv_timeout_ms == 0) {
                    recv_timeout_ms = 1;
                }
                max_frame_len = session->transport.max_plain_frame_len;
                raw_enabled = session->config.enable_raw_asdu != 0;
                layout = get_asdu_layout(*session);
            }
        }
        if (should_stop) {
            dispatch_deferred_async_events(session, layout, raw_callback, user_context);
            complete_worker_stop(session);
            return;
        }

        std::vector<uint8_t> buffer;
        try {
            buffer.resize(max_frame_len);
        } catch (...) {
            change_state(session, IEC_RUNTIME_FAULTED);
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->worker_finished = true;
            }
            session->lifecycle_cv.notify_all();
            return;
        }
        uint32_t received = 0;
        const int recv_result =
            transport.recv(transport.ctx, buffer.data(), max_frame_len, &received, recv_timeout_ms);
        if (recv_result != 0 || received == 0 || received > max_frame_len) {
            if (recv_result != 0 || received > max_frame_len) {
                queue_link_event(session, IEC_LINK_EVENT_LINK_ERROR, IEC_STATUS_IO_ERROR);
            }
            dispatch_pending_timeouts(session);
            dispatch_deferred_async_events(session, layout, raw_callback, user_context);
            continue;
        }

        should_stop = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->stop_requested || session->state != IEC_RUNTIME_RUNNING) {
                should_stop = true;
            }
        }
        if (should_stop) {
            dispatch_deferred_async_events(session, layout, raw_callback, user_context);
            complete_worker_stop(session);
            return;
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
        dispatch_upgrade_control_messages(
            session,
            buffer.data(),
            received,
            layout);
        dispatch_file_messages(
            session,
            buffer.data(),
            received,
            layout,
            file_list_callback,
            file_data_callback,
            file_result_callback,
            user_context);
        dispatch_pending_timeouts(session);
        dispatch_deferred_async_events(session, layout, raw_callback, user_context);
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
    if (is_dangerous_command_semantic(request->semantic)) {
        if (request->command_type != IEC_COMMAND_SINGLE && request->command_type != IEC_COMMAND_DOUBLE) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }
        if (request->mode == IEC_COMMAND_MODE_DIRECT) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }
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
        write_uint_le(frame, frame_size, command_cause(request->mode), 1);
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

        const auto same_target = [&request](const iec_session_t::PendingCommand &pending) {
            return same_command_protocol_key(pending, request->address);
        };
        if (std::any_of(session->pending_commands.begin(), session->pending_commands.end(), same_target)) {
            return IEC_STATUS_BUSY;
        }

        command_id = session->next_command_id++;
        if (session->next_command_id == 0) {
            session->next_command_id = 1;
        }
        timeout_ms = request->timeout_ms != 0 ? request->timeout_ms : session->config.command_timeout_ms;
        session->pending_commands.push_back(iec_session_t::PendingCommand{
            command_id,
            request->semantic,
            request->address,
            type_id,
            command_confirm_cause(request->mode),
            make_deadline(timeout_ms),
        });
        transport = session->transport;
        raw_callback = session->callbacks.on_raw_asdu;
        user_context = session->config.user_context;
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
        queue_raw_asdu_event(session, IEC_RAW_ASDU_TX, frame, frame_size);
    }

    return IEC_STATUS_OK;
}

iec_status_t send_prepared_file_frame(
    iec_session_t *session,
    const uint8_t *frame,
    uint32_t frame_size) noexcept
{
    iec_transport_t transport{};
    uint32_t timeout_ms = 0;
    bool raw_enabled = false;

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_RUNNING) {
            return IEC_STATUS_BAD_STATE;
        }
        if (frame_size > session->transport.max_plain_frame_len) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }
        transport = session->transport;
        timeout_ms = session->config.command_timeout_ms;
        raw_enabled = session->config.enable_raw_asdu != 0;
    }

    const int send_result = transport.send(transport.ctx, frame, frame_size, timeout_ms);
    if (send_result != 0) {
        return IEC_STATUS_IO_ERROR;
    }

    if (raw_enabled) {
        queue_raw_asdu_event(session, IEC_RAW_ASDU_TX, frame, frame_size);
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
        queue_raw_asdu_event(session, IEC_RAW_ASDU_TX, frame, frame_size);
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
        queue_raw_asdu_event(session, IEC_RAW_ASDU_TX, frame, frame_size);
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
        queue_raw_asdu_event(session, IEC_RAW_ASDU_TX, frame, frame_size);
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
        timeout_ms = session->config.command_timeout_ms;
        session->pending_clocks.push_back(iec_session_t::PendingClock{
            request_id,
            IEC_CLOCK_OPERATION_SYNC,
            request->common_address,
            make_deadline(timeout_ms),
        });
        transport = session->transport;
        raw_callback = session->callbacks.on_raw_asdu;
        user_context = session->config.user_context;
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
        queue_raw_asdu_event(session, IEC_RAW_ASDU_TX, frame, frame_size);
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
        timeout_ms = session->config.command_timeout_ms;
        session->pending_clocks.push_back(iec_session_t::PendingClock{
            request_id,
            IEC_CLOCK_OPERATION_READ,
            request->common_address,
            make_deadline(timeout_ms),
        });
        transport = session->transport;
        raw_callback = session->callbacks.on_raw_asdu;
        user_context = session->config.user_context;
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
        queue_raw_asdu_event(session, IEC_RAW_ASDU_TX, frame, frame_size);
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
    iec_parameter_write_mode_t write_mode,
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
        const auto same_target = [operation, common_address, setting_group](
                                     const iec_session_t::PendingParameter &pending) {
            return pending.operation == operation &&
                pending.common_address == common_address &&
                parameter_groups_overlap(pending.setting_group, setting_group);
        };
        if (std::any_of(session->pending_parameters.begin(), session->pending_parameters.end(), same_target)) {
            return IEC_STATUS_BUSY;
        }

        request_id = take_next_request_id(*session);
        timeout_ms = session->config.command_timeout_ms;
        session->pending_parameters.push_back(iec_session_t::PendingParameter{
            request_id,
            operation,
            common_address,
            setting_group,
            write_mode,
            make_deadline(timeout_ms),
        });
        transport = session->transport;
        raw_callback = session->callbacks.on_raw_asdu;
        user_context = session->config.user_context;
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
        queue_raw_asdu_event(session, IEC_RAW_ASDU_TX, frame, frame_size);
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
            0,
            frame,
            frame_size,
            layout)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    frame[frame_size++] = static_cast<uint8_t>(request->read_mode);
    frame[frame_size++] = static_cast<uint8_t>(request->scope);
    write_uint_le(frame, frame_size, request->start_address, 4);
    write_uint_le(frame, frame_size, request->end_address, 4);

    return send_parameter_request(
        session,
        frame,
        frame_size,
        request->common_address,
        request->setting_group,
        IEC_PARAMETER_OPERATION_READ,
        IEC_PARAMETER_WRITE_MODE_NONE,
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
        !validate_parameter_write_request(*request)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    uint8_t frame[512]{};
    uint32_t frame_size = 0;
    AsduLayout layout{};
    const uint8_t flags = static_cast<uint8_t>(static_cast<uint8_t>(request->mode) << 2U);

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

    frame[frame_size++] = static_cast<uint8_t>(request->mode);
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
        request->mode,
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
        IEC_PARAMETER_WRITE_MODE_NONE,
        out_request_id);
}

namespace {

bool begin_file_frame(
    iec_session_t *session,
    iec_file_operation_t operation,
    uint16_t common_address,
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

    const uint8_t type_id = file_type_id(operation);
    if (type_id == 0) {
        return false;
    }

    frame[frame_size++] = type_id;
    frame[frame_size++] = 1;
    write_uint_le(frame, frame_size, operation == IEC_FILE_OPERATION_LIST ? kCauseRequest : kCauseActivation, 1);
    if (layout.cot_length == 2) {
        write_uint_le(frame, frame_size, kDefaultOriginatorAddress, 1);
    }
    write_uint_le(frame, frame_size, common_address, layout.common_address_length);
    write_uint_le(frame, frame_size, kFileChannelInformationObjectAddress, layout.information_object_address_length);
    frame[frame_size++] = flags;
    frame[frame_size++] = 0;
    return true;
}

bool begin_upgrade_control_frame(
    iec_session_t *session,
    iec_upgrade_operation_t operation,
    uint16_t common_address,
    uint32_t information_object_address,
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

    frame[frame_size++] = kAsduTypeUpgradeControl;
    frame[frame_size++] = 1;
    write_uint_le(frame, frame_size, upgrade_control_cause(operation), 1);
    if (layout.cot_length == 2) {
        write_uint_le(frame, frame_size, kDefaultOriginatorAddress, 1);
    }
    write_uint_le(frame, frame_size, common_address, layout.common_address_length);
    write_uint_le(
        frame,
        frame_size,
        information_object_address == 0 ? kUpgradeChannelInformationObjectAddress : information_object_address,
        layout.information_object_address_length);
    frame[frame_size++] = upgrade_control_se_bit(operation);
    return true;
}

iec_status_t send_file_frame(
    iec_session_t *session,
    const uint8_t *frame,
    uint32_t frame_size,
    uint32_t id,
    bool is_transfer,
    uint32_t *out_id) noexcept
{
    iec_transport_t transport{};
    uint32_t timeout_ms = 0;
    bool raw_enabled = false;

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_RUNNING) {
            return IEC_STATUS_BAD_STATE;
        }
        if (frame_size > session->transport.max_plain_frame_len) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }
        transport = session->transport;
        timeout_ms = session->config.command_timeout_ms;
        raw_enabled = session->config.enable_raw_asdu != 0;
    }

    const int send_result = transport.send(transport.ctx, frame, frame_size, timeout_ms);
    if (send_result != 0) {
        queue_link_event(session, IEC_LINK_EVENT_LINK_ERROR, IEC_STATUS_IO_ERROR);
        std::lock_guard<std::mutex> lock(session->mutex);
        if (is_transfer) {
            auto match = std::find_if(
                session->file_transfers.begin(),
                session->file_transfers.end(),
                [id](const iec_session_t::FileTransfer &transfer) {
                    return transfer.transfer_id == id;
                });
            if (match != session->file_transfers.end()) {
                session->file_transfers.erase(match);
            }
        } else {
            auto match = std::find_if(
                session->pending_file_lists.begin(),
                session->pending_file_lists.end(),
                [id](const iec_session_t::PendingFileList &pending) {
                    return pending.request_id == id;
                });
            if (match != session->pending_file_lists.end()) {
                session->pending_file_lists.erase(match);
            }
        }
        return IEC_STATUS_IO_ERROR;
    }

    *out_id = id;
    if (raw_enabled) {
        queue_raw_asdu_event(session, IEC_RAW_ASDU_TX, frame, frame_size);
    }

    return IEC_STATUS_OK;
}

iec_status_t send_upgrade_control_frame(
    iec_session_t *session,
    const uint8_t *frame,
    uint32_t frame_size,
    uint32_t timeout_ms) noexcept
{
    iec_transport_t transport{};
    bool raw_enabled = false;

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_RUNNING) {
            return IEC_STATUS_BAD_STATE;
        }
        if (frame_size > session->transport.max_plain_frame_len) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }
        transport = session->transport;
        raw_enabled = session->config.enable_raw_asdu != 0;
    }

    const int send_result = transport.send(transport.ctx, frame, frame_size, timeout_ms);
    if (send_result != 0) {
        queue_link_event(session, IEC_LINK_EVENT_LINK_ERROR, IEC_STATUS_IO_ERROR);
        return IEC_STATUS_IO_ERROR;
    }
    if (raw_enabled) {
        queue_raw_asdu_event(session, IEC_RAW_ASDU_TX, frame, frame_size);
    }

    return IEC_STATUS_OK;
}

} // namespace

iec_status_t list_files(
    iec_session_t *session,
    const iec_file_list_request_t *request,
    uint32_t *out_request_id) noexcept
{
    if (out_request_id != nullptr) {
        *out_request_id = 0;
    }
    if (session == nullptr || request == nullptr || out_request_id == nullptr ||
        !is_binary_flag(request->include_details) ||
        !is_non_empty_bounded_string(request->directory_name, kMaxFileNameBytes)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    uint8_t frame[192]{};
    uint32_t frame_size = 0;
    AsduLayout layout{};
    if (!begin_file_frame(
            session,
            IEC_FILE_OPERATION_LIST,
            request->common_address,
            static_cast<uint8_t>(request->include_details != 0 ? 0x02U : 0U),
            frame,
            frame_size,
            layout) ||
        !append_string_field(frame, frame_size, sizeof(frame), request->directory_name, kMaxFileNameBytes)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    uint32_t request_id = 0;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_RUNNING) {
            return IEC_STATUS_BAD_STATE;
        }
        request_id = take_next_request_id(*session);
        session->pending_file_lists.push_back(iec_session_t::PendingFileList{
            request_id,
            request->common_address,
            request->directory_name,
            make_deadline(session->config.command_timeout_ms),
        });
    }

    return send_file_frame(session, frame, frame_size, request_id, false, out_request_id);
}

iec_status_t read_file(
    iec_session_t *session,
    const iec_file_read_request_t *request,
    uint32_t *out_transfer_id) noexcept
{
    if (out_transfer_id != nullptr) {
        *out_transfer_id = 0;
    }
    if (session == nullptr || request == nullptr || out_transfer_id == nullptr ||
        !is_non_empty_bounded_string(request->directory_name, kMaxFileNameBytes) ||
        !is_non_empty_bounded_string(request->file_name, kMaxFileNameBytes)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    uint8_t frame[512]{};
    uint32_t frame_size = 0;
    AsduLayout layout{};
    if (!begin_file_frame(session, IEC_FILE_OPERATION_READ, request->common_address, 0, frame, frame_size, layout) ||
        !append_string_field(frame, frame_size, sizeof(frame), request->directory_name, kMaxFileNameBytes) ||
        !append_string_field(frame, frame_size, sizeof(frame), request->file_name, kMaxFileNameBytes)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    uint32_t transfer_id = 0;
    uint32_t chunk_size = 0;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_RUNNING) {
            return IEC_STATUS_BAD_STATE;
        }
        chunk_size = effective_file_read_chunk_size(*session, request->max_chunk_size, layout);
        if (chunk_size == 0 || !fits_uint_le(request->common_address, layout.common_address_length)) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }
        transfer_id = take_next_transfer_id(*session);
        session->file_transfers.push_back(iec_session_t::FileTransfer{
            transfer_id,
            IEC_FILE_TRANSFER_DIRECTION_READ,
            IEC_FILE_TRANSFER_STATE_ACCEPTED,
            request->common_address,
            request->directory_name,
            request->file_name,
            request->expected_file_size,
            request->start_offset,
            1,
            IEC_FILE_RESULT_ACCEPTED,
            0,
            0,
            make_deadline(session->config.command_timeout_ms),
        });
    }

    write_uint_le(frame, frame_size, transfer_id, 4);
    write_uint_le(frame, frame_size, request->start_offset, 4);
    write_uint_le(frame, frame_size, chunk_size, 4);
    write_uint_le(frame, frame_size, request->expected_file_size, 4);

    return send_file_frame(session, frame, frame_size, transfer_id, true, out_transfer_id);
}

iec_status_t write_file(
    iec_session_t *session,
    const iec_file_write_request_t *request,
    uint32_t *out_transfer_id) noexcept
{
    if (out_transfer_id != nullptr) {
        *out_transfer_id = 0;
    }
    if (session == nullptr || request == nullptr || out_transfer_id == nullptr ||
        !is_binary_flag(request->overwrite_existing) ||
        !is_non_empty_bounded_string(request->directory_name, kMaxFileNameBytes) ||
        !is_non_empty_bounded_string(request->file_name, kMaxFileNameBytes) ||
        request->content == nullptr || request->content_size == 0 ||
        request->start_offset > request->total_size ||
        request->content_size > request->total_size - request->start_offset) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    uint8_t header[512]{};
    uint32_t header_size = 0;
    AsduLayout layout{};
    if (!begin_file_frame(
            session,
            IEC_FILE_OPERATION_WRITE,
            request->common_address,
            request->overwrite_existing != 0 ? 0x02U : 0U,
            header,
            header_size,
            layout) ||
        !append_string_field(header, header_size, sizeof(header), request->directory_name, kMaxFileNameBytes) ||
        !append_string_field(header, header_size, sizeof(header), request->file_name, kMaxFileNameBytes)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    uint32_t transfer_id = 0;
    uint32_t chunk_size = 0;
    uint32_t max_frame_len = 0;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_RUNNING) {
            return IEC_STATUS_BAD_STATE;
        }
        chunk_size = effective_file_chunk_size(*session, request->preferred_chunk_size);
        max_frame_len = session->transport.max_plain_frame_len;
        if (chunk_size == 0 || header_size + 20U >= max_frame_len) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }
        chunk_size = std::min<uint32_t>(chunk_size, max_frame_len - header_size - 20U);
        transfer_id = take_next_transfer_id(*session);
        session->file_transfers.push_back(iec_session_t::FileTransfer{
            transfer_id,
            IEC_FILE_TRANSFER_DIRECTION_WRITE,
            IEC_FILE_TRANSFER_STATE_ACCEPTED,
            request->common_address,
            request->directory_name,
            request->file_name,
            request->total_size,
            request->start_offset,
            1,
            IEC_FILE_RESULT_ACCEPTED,
            0,
            0,
            make_deadline(session->config.command_timeout_ms),
        });
    }

    uint32_t sent = 0;
    while (sent < request->content_size) {
        const uint32_t window_size = std::min<uint32_t>(chunk_size, request->content_size - sent);
        uint8_t frame[1536]{};
        uint32_t frame_size = header_size;
        std::memcpy(frame, header, header_size);
        write_uint_le(frame, frame_size, transfer_id, 4);
        write_uint_le(frame, frame_size, request->start_offset + sent, 4);
        write_uint_le(frame, frame_size, request->total_size, 4);
        write_uint_le(frame, frame_size, chunk_size, 4);
        write_uint_le(frame, frame_size, window_size, 4);
        std::memcpy(frame + frame_size, request->content + sent, window_size);
        frame_size += window_size;

        const iec_status_t send_status = send_prepared_file_frame(session, frame, frame_size);
        if (send_status != IEC_STATUS_OK) {
            std::lock_guard<std::mutex> lock(session->mutex);
            auto match = std::find_if(
                session->file_transfers.begin(),
                session->file_transfers.end(),
                [transfer_id](const iec_session_t::FileTransfer &transfer) {
                    return transfer.transfer_id == transfer_id;
                });
            if (match != session->file_transfers.end()) {
                session->file_transfers.erase(match);
            }
            return send_status;
        }
        sent += window_size;
    }

    *out_transfer_id = transfer_id;
    return IEC_STATUS_OK;
}

iec_status_t get_file_transfer_status(
    const iec_session_t *session,
    uint32_t transfer_id,
    iec_file_transfer_status_t *out_status) noexcept
{
    if (out_status != nullptr) {
        *out_status = iec_file_transfer_status_t{};
    }
    if (session == nullptr || transfer_id == 0 || out_status == nullptr) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    std::lock_guard<std::mutex> lock(session->mutex);
    auto match = std::find_if(
        session->file_transfers.begin(),
        session->file_transfers.end(),
        [transfer_id](const iec_session_t::FileTransfer &transfer) {
            return transfer.transfer_id == transfer_id;
        });
    if (match == session->file_transfers.end()) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    out_status->transfer_id = match->transfer_id;
    out_status->direction = match->direction;
    out_status->state = match->state;
    out_status->common_address = match->common_address;
    out_status->directory_name = match->directory_name.c_str();
    out_status->file_name = match->file_name.c_str();
    out_status->total_size = match->total_size;
    out_status->acknowledged_offset = match->acknowledged_offset;
    out_status->is_resumable = match->is_resumable;
    out_status->last_result = match->last_result;
    out_status->last_cause_of_transmission = match->last_cause_of_transmission;
    out_status->last_native_error_code = match->last_native_error_code;
    return IEC_STATUS_OK;
}

iec_status_t upgrade_control(
    iec_session_t *session,
    const iec_upgrade_control_request_t *request,
    uint32_t *out_request_id) noexcept
{
    if (out_request_id != nullptr) {
        *out_request_id = 0;
    }
    if (session == nullptr || request == nullptr || out_request_id == nullptr ||
        !is_upgrade_operation_valid(request->operation)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    uint8_t frame[64]{};
    uint32_t frame_size = 0;
    AsduLayout layout{};
    if (!begin_upgrade_control_frame(
            session,
            request->operation,
            request->common_address,
            request->information_object_address,
            frame,
            frame_size,
            layout)) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }

    uint32_t request_id = 0;
    uint32_t command_timeout_ms = request->command_timeout_ms;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_RUNNING) {
            return IEC_STATUS_BAD_STATE;
        }
        if (!fits_uint_le(request->common_address, layout.common_address_length)) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }
        if (!fits_uint_le(request->information_object_address, layout.information_object_address_length)) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }
        if (command_timeout_ms == 0) {
            command_timeout_ms = session->config.command_timeout_ms;
        }
        const uint32_t effective_information_object_address = request->information_object_address == 0
            ? kUpgradeChannelInformationObjectAddress
            : request->information_object_address;
        const auto same_target = [common_address = request->common_address,
                                  effective_information_object_address](
                                     const iec_session_t::PendingUpgradeControl &pending) {
            const uint32_t pending_information_object_address = pending.information_object_address == 0
                ? kUpgradeChannelInformationObjectAddress
                : pending.information_object_address;
            return pending.common_address == common_address &&
                pending_information_object_address == effective_information_object_address;
        };
        if (std::any_of(
                session->pending_upgrade_controls.begin(),
                session->pending_upgrade_controls.end(),
                same_target)) {
            return IEC_STATUS_BUSY;
        }
        request_id = take_next_request_id(*session);
        session->pending_upgrade_controls.push_back(iec_session_t::PendingUpgradeControl{
            request_id,
            request->common_address,
            effective_information_object_address,
            request->operation,
            make_deadline(command_timeout_ms),
        });
    }

    iec_status_t send_status =
        send_upgrade_control_frame(session, frame, frame_size, command_timeout_ms);
    if (send_status != IEC_STATUS_OK) {
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->pending_upgrade_controls.erase(
                std::remove_if(
                    session->pending_upgrade_controls.begin(),
                    session->pending_upgrade_controls.end(),
                    [request_id](const iec_session_t::PendingUpgradeControl &pending) {
                        return pending.request_id == request_id;
                    }),
                session->pending_upgrade_controls.end());
        }
        queue_link_event(session, IEC_LINK_EVENT_LINK_ERROR, send_status);
        return send_status;
    }

    *out_request_id = request_id;
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
        if (!validate_raw_asdu_base(request->payload, request->payload_size, layout)) {
            return IEC_STATUS_INVALID_ARGUMENT;
        }

        transport = session->transport;
        timeout_ms = session->config.command_timeout_ms;
    }

    const int send_result = transport.send(transport.ctx, request->payload, request->payload_size, timeout_ms);
    if (send_result != 0) {
        queue_link_event(session, IEC_LINK_EVENT_LINK_ERROR, IEC_STATUS_IO_ERROR);
        return IEC_STATUS_IO_ERROR;
    }
    queue_raw_asdu_event(session, IEC_RAW_ASDU_TX, request->payload, request->payload_size);

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
        session->state = IEC_RUNTIME_STARTING;
        session->stop_requested = false;
        session->worker_finished = false;
    }
    session->lifecycle_cv.notify_all();

    try {
        session->worker = std::thread(receive_worker, session);
    } catch (const std::bad_alloc &) {
        set_state_without_callback(session, IEC_RUNTIME_FAULTED);
        return IEC_STATUS_NO_MEMORY;
    } catch (...) {
        set_state_without_callback(session, IEC_RUNTIME_FAULTED);
        return IEC_STATUS_INTERNAL_ERROR;
    }

    const uint32_t timeout_ms = session->config.startup_timeout_ms;
    std::unique_lock<std::mutex> lock(session->mutex);
    if (timeout_ms == 0) {
        session->lifecycle_cv.wait(lock, [session] {
            return session->state != IEC_RUNTIME_STARTING;
        });
    } else if (!session->lifecycle_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [session] {
                   return session->state != IEC_RUNTIME_STARTING;
               })) {
        return IEC_STATUS_TIMEOUT;
    }
    return session->state == IEC_RUNTIME_RUNNING ? IEC_STATUS_OK : IEC_STATUS_INTERNAL_ERROR;
}

iec_status_t stop_session(iec_session_t *session, uint32_t timeout_ms) noexcept
{
    if (session == nullptr) {
        return IEC_STATUS_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->state != IEC_RUNTIME_RUNNING && session->state != IEC_RUNTIME_STOPPING) {
            return IEC_STATUS_BAD_STATE;
        }
        session->stop_requested = true;
        session->state = IEC_RUNTIME_STOPPING;
    }
    session->lifecycle_cv.notify_all();

    const uint32_t wait_ms = timeout_ms != 0 ? timeout_ms : session->config.stop_timeout_ms;
    {
        std::unique_lock<std::mutex> lock(session->mutex);
        if (wait_ms == 0) {
            session->lifecycle_cv.wait(lock, [session] {
                return session->worker_finished;
            });
        } else if (!session->lifecycle_cv.wait_for(lock, std::chrono::milliseconds(wait_ms), [session] {
                       return session->worker_finished;
                   })) {
            return IEC_STATUS_TIMEOUT;
        }
    }
    if (session->worker.joinable()) {
        session->worker.join();
    }
    return IEC_STATUS_OK;
}

} // namespace gw::protocol
