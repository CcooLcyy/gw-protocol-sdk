#ifndef GW_PROTOCOL_SESSION_HPP
#define GW_PROTOCOL_SESSION_HPP

#include "gw_iec101.h"
#include "gw_iec104.h"
#include "gw_m101.h"

namespace gw::protocol {

enum class Profile {
    M101,
    IEC101,
    IEC104
};

iec_status_t validate_m101_config(const m101_master_config_t *config) noexcept;
iec_status_t validate_iec101_config(const iec101_master_config_t *config) noexcept;
iec_status_t validate_iec104_config(const iec104_master_config_t *config) noexcept;

iec_status_t create_session(
    Profile profile,
    const iec_session_config_t *config,
    const void *protocol_config,
    const iec_transport_t *transport,
    const iec_callbacks_t *callbacks,
    iec_session_t **out_session) noexcept;

iec_status_t destroy_session(iec_session_t *session) noexcept;
iec_status_t get_runtime_state(const iec_session_t *session, iec_runtime_state_t *out_state) noexcept;
iec_status_t general_interrogation(iec_session_t *session, const iec_interrogation_request_t *request) noexcept;
iec_status_t set_option(iec_session_t *session, iec_option_t option, const void *value, uint32_t value_size) noexcept;
iec_status_t send_raw_asdu(iec_session_t *session, const iec_raw_asdu_tx_t *request) noexcept;
iec_status_t start_session(iec_session_t *session) noexcept;
iec_status_t stop_session(iec_session_t *session, uint32_t timeout_ms) noexcept;

} // namespace gw::protocol

#endif
