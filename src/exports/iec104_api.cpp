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
