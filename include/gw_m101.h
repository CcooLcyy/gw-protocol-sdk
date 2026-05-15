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

typedef struct iec_callbacks {
    iec_on_session_state_fn on_session_state;
    iec_on_point_indication_fn on_point_indication;
    iec_on_command_result_fn on_command_result;
    iec_on_raw_asdu_fn on_raw_asdu;
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
