#define GW_PROTOCOL_SDK_NO_API_DECLARATIONS
#include "core/session.hpp"

extern "C" {

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_validate_config(const iec104_master_config_t *config)
{
    return gw::protocol::validate_iec104_config(config);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_create(
    const iec_session_config_t *config,
    const void *protocol_config,
    const iec_transport_t *transport,
    const iec_callbacks_t *callbacks,
    iec_session_t **out_session)
{
    return gw::protocol::create_session(
        gw::protocol::Profile::IEC104, config, protocol_config, transport, callbacks, out_session);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_destroy(iec_session_t *session)
{
    return gw::protocol::destroy_session(session);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_get_runtime_state(
    const iec_session_t *session,
    iec_runtime_state_t *out_state)
{
    return gw::protocol::get_runtime_state(session, out_state);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_general_interrogation(
    iec_session_t *session,
    const iec_interrogation_request_t *request)
{
    return gw::protocol::general_interrogation(session, request);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_counter_interrogation(
    iec_session_t *session,
    const iec_counter_interrogation_request_t *request)
{
    return gw::protocol::counter_interrogation(session, request);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_control_point(
    iec_session_t *session,
    const iec_command_request_t *request,
    uint32_t *out_command_id)
{
    return gw::protocol::control_point(session, request, out_command_id);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_read_point(
    iec_session_t *session,
    const iec_point_address_t *address)
{
    return gw::protocol::read_point(session, address);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_clock_sync(
    iec_session_t *session,
    const iec_clock_sync_request_t *request,
    uint32_t *out_request_id)
{
    return gw::protocol::clock_sync(session, request, out_request_id);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_read_clock(
    iec_session_t *session,
    const iec_clock_read_request_t *request,
    uint32_t *out_request_id)
{
    return gw::protocol::read_clock(session, request, out_request_id);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_read_parameters(
    iec_session_t *session,
    const iec_parameter_read_request_t *request,
    uint32_t *out_request_id)
{
    return gw::protocol::read_parameters(session, request, out_request_id);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_write_parameters(
    iec_session_t *session,
    const iec_parameter_write_request_t *request,
    uint32_t *out_request_id)
{
    return gw::protocol::write_parameters(session, request, out_request_id);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_verify_parameters(
    iec_session_t *session,
    const iec_parameter_verify_request_t *request,
    uint32_t *out_request_id)
{
    return gw::protocol::verify_parameters(session, request, out_request_id);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_switch_setting_group(
    iec_session_t *session,
    const iec_setting_group_request_t *request,
    uint32_t *out_request_id)
{
    return gw::protocol::switch_setting_group(session, request, out_request_id);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_list_files(
    iec_session_t *session,
    const iec_file_list_request_t *request,
    uint32_t *out_request_id)
{
    return gw::protocol::list_files(session, request, out_request_id);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_read_file(
    iec_session_t *session,
    const iec_file_read_request_t *request,
    uint32_t *out_transfer_id)
{
    return gw::protocol::read_file(session, request, out_transfer_id);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_write_file(
    iec_session_t *session,
    const iec_file_write_request_t *request,
    uint32_t *out_transfer_id)
{
    return gw::protocol::write_file(session, request, out_transfer_id);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_get_file_transfer_status(
    const iec_session_t *session,
    uint32_t transfer_id,
    iec_file_transfer_status_t *out_status)
{
    return gw::protocol::get_file_transfer_status(session, transfer_id, out_status);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_cancel_file_transfer(
    iec_session_t *session,
    uint32_t transfer_id)
{
    return gw::protocol::cancel_file_transfer(session, transfer_id);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_set_option(
    iec_session_t *session,
    iec_option_t option,
    const void *value,
    uint32_t value_size)
{
    return gw::protocol::set_option(session, option, value, value_size);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_send_raw_asdu(
    iec_session_t *session,
    const iec_raw_asdu_tx_t *request)
{
    return gw::protocol::send_raw_asdu(session, request);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_start(iec_session_t *session)
{
    return gw::protocol::start_session(session);
}

GW_PROTOCOL_EXPORT iec_status_t GW_PROTOCOL_CALL iec104_stop(iec_session_t *session, uint32_t timeout_ms)
{
    return gw::protocol::stop_session(session, timeout_ms);
}

}
