#ifndef GW_M101_H
#define GW_M101_H

#include <stdint.h>

#ifndef GW_PROTOCOL_COMMON_DECLARATIONS
#define GW_PROTOCOL_COMMON_DECLARATIONS

#if defined(_WIN32)
#if defined(GW_PROTOCOL_SDK_IMPORT)
#define GW_PROTOCOL_API __declspec(dllimport)
#else
#define GW_PROTOCOL_API
#endif
#define GW_PROTOCOL_EXPORT __declspec(dllexport)
#define GW_PROTOCOL_CALL __cdecl
#else
#define GW_PROTOCOL_API
#define GW_PROTOCOL_EXPORT __attribute__((visibility("default")))
#define GW_PROTOCOL_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iec_session iec_session_t;

typedef enum iec_runtime_state {
    IEC_RUNTIME_CREATED = 0,
    IEC_RUNTIME_STARTING = 1,
    IEC_RUNTIME_RUNNING = 2,
    IEC_RUNTIME_STOPPING = 3,
    IEC_RUNTIME_STOPPED = 4,
    IEC_RUNTIME_FAULTED = 5
} iec_runtime_state_t;

typedef enum iec_status {
    IEC_STATUS_OK = 0,
    IEC_STATUS_INVALID_ARGUMENT = 1,
    IEC_STATUS_UNSUPPORTED = 2,
    IEC_STATUS_BAD_STATE = 3,
    IEC_STATUS_TIMEOUT = 4,
    IEC_STATUS_NO_MEMORY = 5,
    IEC_STATUS_IO_ERROR = 6,
    IEC_STATUS_PROTOCOL_ERROR = 7,
    IEC_STATUS_BUSY = 8,
    IEC_STATUS_INTERNAL_ERROR = 9
} iec_status_t;

typedef enum iec_option {
    IEC_OPTION_LOG_LEVEL = 1,
    IEC_OPTION_RECONNECT_INTERVAL_MS = 2,
    IEC_OPTION_COMMAND_TIMEOUT_MS = 3,
    IEC_OPTION_ENABLE_RAW_ASDU = 4
} iec_option_t;

typedef enum iec_log_level {
    IEC_LOG_ERROR = 1,
    IEC_LOG_WARN = 2,
    IEC_LOG_INFO = 3,
    IEC_LOG_DEBUG = 4
} iec_log_level_t;

typedef struct iec_point_address {
    uint16_t common_address;
    uint32_t information_object_address;
    uint8_t type_id;
    uint8_t cause_of_transmission;
    uint8_t originator_address;
} iec_point_address_t;

typedef enum iec_point_type {
    IEC_POINT_SINGLE = 1,
    IEC_POINT_DOUBLE = 2,
    IEC_POINT_STEP = 3,
    IEC_POINT_MEASURED_NORMALIZED = 4,
    IEC_POINT_MEASURED_SCALED = 5,
    IEC_POINT_MEASURED_SHORT_FLOAT = 6,
    IEC_POINT_INTEGRATED_TOTAL = 7,
    IEC_POINT_BITSTRING32 = 8
} iec_point_type_t;

typedef struct iec_timestamp {
    uint16_t msec;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint8_t invalid;
} iec_timestamp_t;

typedef union iec_point_data {
    uint8_t single;
    uint8_t doubled;
    int8_t step;
    int16_t normalized;
    int16_t scaled;
    float short_float;
    int32_t integrated_total;
    uint32_t bitstring32;
} iec_point_data_t;

typedef struct iec_point_value {
    iec_point_type_t point_type;
    uint8_t quality;
    uint8_t has_timestamp;
    uint8_t is_sequence;
    iec_point_data_t data;
    iec_timestamp_t timestamp;
} iec_point_value_t;

typedef enum iec_command_type {
    IEC_COMMAND_SINGLE = 1,
    IEC_COMMAND_DOUBLE = 2,
    IEC_COMMAND_STEP = 3,
    IEC_COMMAND_SETPOINT_SCALED = 4,
    IEC_COMMAND_SETPOINT_FLOAT = 5,
    IEC_COMMAND_SETPOINT_NORMALIZED = 6
} iec_command_type_t;

typedef enum iec_command_semantic {
    IEC_COMMAND_SEMANTIC_GENERAL = 0,
    IEC_COMMAND_SEMANTIC_FACTORY_RESET = 1,
    IEC_COMMAND_SEMANTIC_DEVICE_REBOOT = 2
} iec_command_semantic_t;

typedef enum iec_command_mode {
    IEC_COMMAND_MODE_DIRECT = 1,
    IEC_COMMAND_MODE_SELECT = 2,
    IEC_COMMAND_MODE_EXECUTE = 3,
    IEC_COMMAND_MODE_CANCEL = 4
} iec_command_mode_t;

typedef union iec_command_data {
    uint8_t single;
    uint8_t doubled;
    int16_t normalized;
    int16_t scaled;
    float short_float;
    int8_t step;
} iec_command_data_t;

typedef struct iec_command_request {
    iec_point_address_t address;
    iec_command_type_t command_type;
    iec_command_semantic_t semantic;
    iec_command_mode_t mode;
    uint8_t qualifier;
    uint8_t execute_on_ack;
    uint16_t timeout_ms;
    iec_command_data_t value;
} iec_command_request_t;

typedef struct iec_interrogation_request {
    uint16_t common_address;
    uint8_t qualifier;
} iec_interrogation_request_t;

typedef struct iec_counter_interrogation_request {
    uint16_t common_address;
    uint8_t qualifier;
    uint8_t freeze;
} iec_counter_interrogation_request_t;

typedef struct iec_clock_sync_request {
    uint16_t common_address;
    uint8_t use_current_system_time;
    iec_timestamp_t timestamp;
} iec_clock_sync_request_t;

typedef struct iec_clock_read_request {
    uint16_t common_address;
} iec_clock_read_request_t;

typedef enum iec_clock_operation {
    IEC_CLOCK_OPERATION_SYNC = 1,
    IEC_CLOCK_OPERATION_READ = 2
} iec_clock_operation_t;

typedef enum iec_clock_result_code {
    IEC_CLOCK_RESULT_ACCEPTED = 1,
    IEC_CLOCK_RESULT_REJECTED = 2,
    IEC_CLOCK_RESULT_TIMEOUT = 3,
    IEC_CLOCK_RESULT_NEGATIVE_CONFIRM = 4,
    IEC_CLOCK_RESULT_PROTOCOL_ERROR = 5,
    IEC_CLOCK_RESULT_UNSUPPORTED = 6
} iec_clock_result_code_t;

typedef struct iec_clock_result {
    uint32_t request_id;
    iec_clock_operation_t operation;
    iec_clock_result_code_t result;
    uint16_t common_address;
    uint8_t has_timestamp;
    iec_timestamp_t timestamp;
    uint8_t cause_of_transmission;
    int32_t native_error_code;
    const char *detail_message;
} iec_clock_result_t;

typedef enum iec_parameter_scope {
    IEC_PARAMETER_SCOPE_ALL = 0,
    IEC_PARAMETER_SCOPE_FIXED = 1,
    IEC_PARAMETER_SCOPE_RUNNING = 2,
    IEC_PARAMETER_SCOPE_ACTION = 3,
    IEC_PARAMETER_SCOPE_WIRELESS = 4,
    IEC_PARAMETER_SCOPE_POWER = 5,
    IEC_PARAMETER_SCOPE_LINE_LOSS = 6,
    IEC_PARAMETER_SCOPE_POINT_TABLE = 7
} iec_parameter_scope_t;

typedef enum iec_parameter_access {
    IEC_PARAMETER_ACCESS_READ_ONLY = 1,
    IEC_PARAMETER_ACCESS_WRITE_ONLY = 2,
    IEC_PARAMETER_ACCESS_READ_WRITE = 3
} iec_parameter_access_t;

typedef enum iec_parameter_value_type {
    IEC_PARAMETER_VALUE_BOOL = 1,
    IEC_PARAMETER_VALUE_INT = 2,
    IEC_PARAMETER_VALUE_UINT = 3,
    IEC_PARAMETER_VALUE_FLOAT = 4,
    IEC_PARAMETER_VALUE_ENUM = 5,
    IEC_PARAMETER_VALUE_STRING = 6
} iec_parameter_value_type_t;

typedef union iec_parameter_scalar {
    uint8_t bool_value;
    int32_t int_value;
    uint32_t uint_value;
    float float_value;
    uint32_t enum_value;
    const char *string_value;
} iec_parameter_scalar_t;

typedef struct iec_parameter_item {
    uint32_t parameter_id;
    uint32_t address;
    iec_parameter_scope_t scope;
    iec_parameter_value_type_t value_type;
    iec_parameter_scalar_t value;
} iec_parameter_item_t;

typedef struct iec_parameter_descriptor {
    uint32_t parameter_id;
    uint32_t address;
    iec_parameter_scope_t scope;
    iec_parameter_value_type_t value_type;
    iec_parameter_access_t access;
    const char *name;
    const char *group_name;
    const char *unit;
    double min_value;
    double max_value;
    double step_value;
    const char *default_value_text;
    uint8_t supports_template;
    uint8_t supports_verify;
} iec_parameter_descriptor_t;

typedef enum iec_parameter_read_mode {
    IEC_PARAMETER_READ_ALL = 1,
    IEC_PARAMETER_READ_BY_SCOPE = 2,
    IEC_PARAMETER_READ_BY_GROUP = 3,
    IEC_PARAMETER_READ_BY_ADDRESS_RANGE = 4
} iec_parameter_read_mode_t;

typedef struct iec_parameter_read_request {
    uint16_t common_address;
    iec_parameter_read_mode_t read_mode;
    iec_parameter_scope_t scope;
    const char *group_name;
    uint32_t start_address;
    uint32_t end_address;
    uint8_t setting_group;
    uint8_t include_descriptor;
} iec_parameter_read_request_t;

typedef struct iec_parameter_write_request {
    uint16_t common_address;
    uint8_t setting_group;
    const iec_parameter_item_t *items;
    uint32_t item_count;
    uint8_t verify_after_write;
} iec_parameter_write_request_t;

typedef struct iec_parameter_verify_request {
    uint16_t common_address;
    uint8_t setting_group;
    const iec_parameter_item_t *expected_items;
    uint32_t item_count;
} iec_parameter_verify_request_t;

typedef enum iec_setting_group_action {
    IEC_SETTING_GROUP_ACTION_GET_CURRENT = 1,
    IEC_SETTING_GROUP_ACTION_SWITCH = 2
} iec_setting_group_action_t;

typedef struct iec_setting_group_request {
    uint16_t common_address;
    iec_setting_group_action_t action;
    uint8_t target_group;
} iec_setting_group_request_t;

typedef enum iec_parameter_operation {
    IEC_PARAMETER_OPERATION_READ = 1,
    IEC_PARAMETER_OPERATION_WRITE = 2,
    IEC_PARAMETER_OPERATION_VERIFY = 3,
    IEC_PARAMETER_OPERATION_SWITCH_GROUP = 4
} iec_parameter_operation_t;

typedef enum iec_parameter_result_code {
    IEC_PARAMETER_RESULT_ACCEPTED = 1,
    IEC_PARAMETER_RESULT_REJECTED = 2,
    IEC_PARAMETER_RESULT_VERIFY_OK = 3,
    IEC_PARAMETER_RESULT_VERIFY_MISMATCH = 4,
    IEC_PARAMETER_RESULT_READ_ONLY = 5,
    IEC_PARAMETER_RESULT_OUT_OF_RANGE = 6,
    IEC_PARAMETER_RESULT_GROUP_SWITCHED = 7,
    IEC_PARAMETER_RESULT_TIMEOUT = 8,
    IEC_PARAMETER_RESULT_PROTOCOL_ERROR = 9,
    IEC_PARAMETER_RESULT_CURRENT_GROUP = 10
} iec_parameter_result_code_t;

typedef struct iec_parameter_indication {
    uint32_t request_id;
    iec_parameter_operation_t operation;
    uint8_t setting_group;
    uint8_t is_final;
    uint8_t has_descriptor;
    iec_parameter_item_t item;
    iec_parameter_descriptor_t descriptor;
} iec_parameter_indication_t;

typedef struct iec_parameter_result {
    uint32_t request_id;
    iec_parameter_operation_t operation;
    iec_parameter_result_code_t result;
    uint32_t parameter_id;
    uint32_t address;
    uint8_t setting_group;
    uint8_t is_final;
} iec_parameter_result_t;

typedef enum iec_device_description_format {
    IEC_DEVICE_DESCRIPTION_FORMAT_AUTO = 0,
    IEC_DEVICE_DESCRIPTION_FORMAT_XML = 1,
    IEC_DEVICE_DESCRIPTION_FORMAT_MSG = 2
} iec_device_description_format_t;

typedef struct iec_device_description_request {
    uint16_t common_address;
    iec_device_description_format_t preferred_format;
    uint32_t max_content_size;
} iec_device_description_request_t;

typedef struct iec_device_description {
    uint32_t request_id;
    uint16_t common_address;
    iec_device_description_format_t format;
    const uint8_t *content;
    uint32_t content_size;
    uint8_t is_complete;
} iec_device_description_t;

typedef enum iec_file_operation {
    IEC_FILE_OPERATION_LIST = 1,
    IEC_FILE_OPERATION_READ = 2,
    IEC_FILE_OPERATION_WRITE = 3,
    IEC_FILE_OPERATION_CANCEL = 4
} iec_file_operation_t;

typedef enum iec_file_transfer_direction {
    IEC_FILE_TRANSFER_DIRECTION_READ = 1,
    IEC_FILE_TRANSFER_DIRECTION_WRITE = 2
} iec_file_transfer_direction_t;

typedef enum iec_file_transfer_state {
    IEC_FILE_TRANSFER_STATE_ACCEPTED = 1,
    IEC_FILE_TRANSFER_STATE_RUNNING = 2,
    IEC_FILE_TRANSFER_STATE_COMPLETED = 3,
    IEC_FILE_TRANSFER_STATE_CANCELED = 4,
    IEC_FILE_TRANSFER_STATE_FAILED = 5
} iec_file_transfer_state_t;

typedef enum iec_file_result_code {
    IEC_FILE_RESULT_ACCEPTED = 1,
    IEC_FILE_RESULT_COMPLETED = 2,
    IEC_FILE_RESULT_CANCELED = 3,
    IEC_FILE_RESULT_REJECTED = 4,
    IEC_FILE_RESULT_NEGATIVE_CONFIRM = 5,
    IEC_FILE_RESULT_OFFSET_MISMATCH = 6,
    IEC_FILE_RESULT_TIMEOUT = 7,
    IEC_FILE_RESULT_PROTOCOL_ERROR = 8,
    IEC_FILE_RESULT_NOT_FOUND = 9
} iec_file_result_code_t;

typedef struct iec_file_list_request {
    uint16_t common_address;
    const char *directory_name;
    uint8_t include_details;
} iec_file_list_request_t;

typedef struct iec_file_entry {
    const char *directory_name;
    const char *file_name;
    uint32_t file_size;
    uint64_t modified_timestamp_ms;
    uint8_t is_directory;
    uint8_t is_read_only;
    const char *checksum_text;
} iec_file_entry_t;

typedef struct iec_file_list_indication {
    uint32_t request_id;
    uint16_t common_address;
    const char *directory_name;
    const iec_file_entry_t *entries;
    uint32_t entry_count;
    uint8_t is_final;
} iec_file_list_indication_t;

typedef struct iec_file_read_request {
    uint16_t common_address;
    const char *directory_name;
    const char *file_name;
    uint32_t start_offset;
    uint32_t max_chunk_size;
    uint32_t expected_file_size;
} iec_file_read_request_t;

typedef struct iec_file_write_request {
    uint16_t common_address;
    const char *directory_name;
    const char *file_name;
    uint32_t start_offset;
    uint32_t total_size;
    const uint8_t *content;
    uint32_t content_size;
    uint32_t preferred_chunk_size;
    uint8_t overwrite_existing;
} iec_file_write_request_t;

typedef struct iec_file_data_indication {
    uint32_t transfer_id;
    iec_file_transfer_direction_t direction;
    uint16_t common_address;
    const char *directory_name;
    const char *file_name;
    uint32_t total_size;
    uint32_t current_offset;
    uint32_t next_offset;
    const uint8_t *data;
    uint32_t data_size;
    uint8_t is_final;
} iec_file_data_indication_t;

typedef struct iec_file_transfer_status {
    uint32_t transfer_id;
    iec_file_transfer_direction_t direction;
    iec_file_transfer_state_t state;
    uint16_t common_address;
    const char *directory_name;
    const char *file_name;
    uint32_t total_size;
    uint32_t acknowledged_offset;
    uint8_t is_resumable;
    iec_file_result_code_t last_result;
    uint8_t last_cause_of_transmission;
    int32_t last_native_error_code;
} iec_file_transfer_status_t;

typedef struct iec_file_operation_result {
    uint32_t request_id;
    uint32_t transfer_id;
    iec_file_operation_t operation;
    iec_file_transfer_direction_t direction;
    iec_file_result_code_t result;
    uint16_t common_address;
    const char *directory_name;
    const char *file_name;
    uint32_t final_offset;
    uint32_t total_size;
    uint8_t cause_of_transmission;
    int32_t native_error_code;
    const char *detail_message;
    uint8_t is_final;
} iec_file_operation_result_t;

typedef enum iec_raw_asdu_direction {
    IEC_RAW_ASDU_RX = 1,
    IEC_RAW_ASDU_TX = 2
} iec_raw_asdu_direction_t;

typedef struct iec_raw_asdu_event {
    iec_raw_asdu_direction_t direction;
    uint16_t common_address;
    uint8_t type_id;
    uint8_t cause_of_transmission;
    const uint8_t *payload;
    uint32_t payload_size;
    uint64_t monotonic_ns;
} iec_raw_asdu_event_t;

typedef struct iec_raw_asdu_tx {
    const uint8_t *payload;
    uint32_t payload_size;
    uint8_t bypass_high_level_validation;
} iec_raw_asdu_tx_t;

typedef enum iec_command_result_code {
    IEC_COMMAND_RESULT_ACCEPTED = 1,
    IEC_COMMAND_RESULT_REJECTED = 2,
    IEC_COMMAND_RESULT_TIMEOUT = 3,
    IEC_COMMAND_RESULT_NEGATIVE_CONFIRM = 4,
    IEC_COMMAND_RESULT_PROTOCOL_ERROR = 5
} iec_command_result_code_t;

typedef struct iec_command_result {
    uint32_t command_id;
    iec_command_semantic_t semantic;
    iec_command_result_code_t result;
    iec_point_address_t address;
    uint8_t is_final;
} iec_command_result_t;

typedef int(GW_PROTOCOL_CALL *iec_transport_send_fn)(
    void *ctx,
    const uint8_t *data,
    uint32_t len,
    uint32_t timeout_ms);

typedef int(GW_PROTOCOL_CALL *iec_transport_recv_fn)(
    void *ctx,
    uint8_t *buffer,
    uint32_t capacity,
    uint32_t *out_len,
    uint32_t timeout_ms);

typedef struct iec_transport {
    iec_transport_send_fn send;
    iec_transport_recv_fn recv;
    void *ctx;
    uint32_t max_plain_frame_len;
} iec_transport_t;

typedef struct iec_session_config {
    void *user_context;
    uint32_t startup_timeout_ms;
    uint32_t stop_timeout_ms;
    uint32_t reconnect_interval_ms;
    uint32_t command_timeout_ms;
    uint8_t enable_raw_asdu;
    uint8_t enable_log_callback;
    uint8_t initial_log_level;
} iec_session_config_t;

typedef void(GW_PROTOCOL_CALL *iec_on_session_state_fn)(
    iec_session_t *session,
    iec_runtime_state_t state,
    void *user_context);

typedef void(GW_PROTOCOL_CALL *iec_on_point_indication_fn)(
    iec_session_t *session,
    const iec_point_address_t *address,
    const iec_point_value_t *value,
    void *user_context);

typedef void(GW_PROTOCOL_CALL *iec_on_command_result_fn)(
    iec_session_t *session,
    const iec_command_result_t *result,
    void *user_context);

typedef void(GW_PROTOCOL_CALL *iec_on_raw_asdu_fn)(
    iec_session_t *session,
    const iec_raw_asdu_event_t *event,
    void *user_context);

typedef void(GW_PROTOCOL_CALL *iec_on_clock_result_fn)(
    iec_session_t *session,
    const iec_clock_result_t *result,
    void *user_context);

typedef void(GW_PROTOCOL_CALL *iec_on_parameter_indication_fn)(
    iec_session_t *session,
    const iec_parameter_indication_t *indication,
    void *user_context);

typedef void(GW_PROTOCOL_CALL *iec_on_parameter_result_fn)(
    iec_session_t *session,
    const iec_parameter_result_t *result,
    void *user_context);

typedef void(GW_PROTOCOL_CALL *iec_on_device_description_fn)(
    iec_session_t *session,
    const iec_device_description_t *description,
    void *user_context);

typedef void(GW_PROTOCOL_CALL *iec_on_file_list_indication_fn)(
    iec_session_t *session,
    const iec_file_list_indication_t *indication,
    void *user_context);

typedef void(GW_PROTOCOL_CALL *iec_on_file_data_indication_fn)(
    iec_session_t *session,
    const iec_file_data_indication_t *indication,
    void *user_context);

typedef void(GW_PROTOCOL_CALL *iec_on_file_operation_result_fn)(
    iec_session_t *session,
    const iec_file_operation_result_t *result,
    void *user_context);

typedef struct iec_callbacks {
    iec_on_session_state_fn on_session_state;
    iec_on_point_indication_fn on_point_indication;
    iec_on_command_result_fn on_command_result;
    iec_on_raw_asdu_fn on_raw_asdu;
    iec_on_clock_result_fn on_clock_result;
    iec_on_parameter_indication_fn on_parameter_indication;
    iec_on_parameter_result_fn on_parameter_result;
    iec_on_file_list_indication_fn on_file_list_indication;
    iec_on_file_data_indication_fn on_file_data_indication;
    iec_on_file_operation_result_fn on_file_operation_result;
    iec_on_device_description_fn on_device_description;
} iec_callbacks_t;

#ifdef __cplusplus
}
#endif

#endif

#ifndef GW_PROTOCOL_IEC101_LINK_MODE_DECLARATION
#define GW_PROTOCOL_IEC101_LINK_MODE_DECLARATION

#ifdef __cplusplus
extern "C" {
#endif

typedef enum iec101_link_mode {
    IEC101_LINK_MODE_UNBALANCED = 1,
    IEC101_LINK_MODE_BALANCED = 2
} iec101_link_mode_t;

#ifdef __cplusplus
}
#endif

#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct m101_master_config {
    iec101_link_mode_t link_mode;
    uint16_t link_address;
    uint8_t link_address_length;
    uint8_t common_address_length;
    uint8_t information_object_address_length;
    uint8_t cot_length;
    uint8_t use_single_char_ack;
    uint32_t ack_timeout_ms;
    uint32_t repeat_timeout_ms;
    uint32_t repeat_count;
    uint32_t preferred_file_chunk_size;
} m101_master_config_t;

#ifndef GW_PROTOCOL_SDK_NO_API_DECLARATIONS
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_create(
    const iec_session_config_t *config,
    const void *protocol_config,
    const iec_transport_t *transport,
    const iec_callbacks_t *callbacks,
    iec_session_t **out_session);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_destroy(iec_session_t *session);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_get_runtime_state(
    const iec_session_t *session,
    iec_runtime_state_t *out_state);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_general_interrogation(
    iec_session_t *session,
    const iec_interrogation_request_t *request);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_counter_interrogation(
    iec_session_t *session,
    const iec_counter_interrogation_request_t *request);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_control_point(
    iec_session_t *session,
    const iec_command_request_t *request,
    uint32_t *out_command_id);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_read_point(
    iec_session_t *session,
    const iec_point_address_t *address);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_clock_sync(
    iec_session_t *session,
    const iec_clock_sync_request_t *request,
    uint32_t *out_request_id);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_read_clock(
    iec_session_t *session,
    const iec_clock_read_request_t *request,
    uint32_t *out_request_id);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_read_parameters(
    iec_session_t *session,
    const iec_parameter_read_request_t *request,
    uint32_t *out_request_id);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_write_parameters(
    iec_session_t *session,
    const iec_parameter_write_request_t *request,
    uint32_t *out_request_id);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_verify_parameters(
    iec_session_t *session,
    const iec_parameter_verify_request_t *request,
    uint32_t *out_request_id);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_switch_setting_group(
    iec_session_t *session,
    const iec_setting_group_request_t *request,
    uint32_t *out_request_id);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_get_device_description(
    iec_session_t *session,
    const iec_device_description_request_t *request,
    uint32_t *out_request_id);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_list_files(
    iec_session_t *session,
    const iec_file_list_request_t *request,
    uint32_t *out_request_id);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_read_file(
    iec_session_t *session,
    const iec_file_read_request_t *request,
    uint32_t *out_transfer_id);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_write_file(
    iec_session_t *session,
    const iec_file_write_request_t *request,
    uint32_t *out_transfer_id);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_get_file_transfer_status(
    const iec_session_t *session,
    uint32_t transfer_id,
    iec_file_transfer_status_t *out_status);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_cancel_file_transfer(
    iec_session_t *session,
    uint32_t transfer_id);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_set_option(
    iec_session_t *session,
    iec_option_t option,
    const void *value,
    uint32_t value_size);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_send_raw_asdu(
    iec_session_t *session,
    const iec_raw_asdu_tx_t *request);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_start(iec_session_t *session);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_stop(iec_session_t *session, uint32_t timeout_ms);
GW_PROTOCOL_API iec_status_t GW_PROTOCOL_CALL m101_validate_config(const m101_master_config_t *config);
#endif

#ifdef __cplusplus
}
#endif

#endif
