#pragma once
#include <cstdint>
#define BLE_UUID_TYPE_128 128
#define BLE_HS_EDONE 14
#define BLE_HS_IO_NO_INPUT_OUTPUT 3
#define BLE_SM_PAIR_KEY_DIST_ENC 1
#define BLE_SM_PAIR_KEY_DIST_ID 2
#define BLE_GAP_EVENT_ENC_CHANGE 10
#define BLE_GAP_EVENT_NOTIFY_RX 12
#define BLE_GAP_EVENT_DISCONNECT 1
struct ble_uuid_t { uint8_t type; };
struct ble_uuid128_t { ble_uuid_t u; uint8_t value[16]; };
union ble_uuid_any_t { ble_uuid_t u; ble_uuid128_t u128; };
struct ble_gatt_error { uint16_t status; uint16_t att_handle; };
struct ble_gatt_svc { uint16_t start_handle, end_handle; ble_uuid_any_t uuid; };
struct ble_gatt_chr { uint16_t def_handle, val_handle; uint8_t properties; ble_uuid_any_t uuid; };
struct ble_gatt_dsc { uint16_t handle; ble_uuid_any_t uuid; };
struct ble_gatt_attr { uint16_t handle; };
struct os_mbuf { int dummy; };
#define OS_MBUF_PKTLEN(om) 0
inline int os_mbuf_copydata(const os_mbuf*, int, int, void*){return 0;}
struct ble_gap_event { int type; struct { int status; uint16_t conn_handle; } enc_change; struct { uint16_t conn_handle, attr_handle; os_mbuf* om; } notify_rx; };
typedef int ble_gap_event_fn(ble_gap_event*, void*);
struct ble_gap_event_listener { int x; };
inline int ble_gap_event_listener_register(ble_gap_event_listener*, ble_gap_event_fn*, void*){return 0;}
inline int ble_uuid_cmp(const ble_uuid_t*, const ble_uuid_t*){return 1;}
inline uint16_t ble_uuid_u16(const ble_uuid_t*){return 0;}
typedef int ble_gatt_disc_svc_fn(uint16_t, const ble_gatt_error*, const ble_gatt_svc*, void*);
typedef int ble_gatt_chr_fn(uint16_t, const ble_gatt_error*, const ble_gatt_chr*, void*);
typedef int ble_gatt_dsc_fn(uint16_t, const ble_gatt_error*, uint16_t, const ble_gatt_dsc*, void*);
typedef int ble_gatt_attr_fn(uint16_t, const ble_gatt_error*, ble_gatt_attr*, void*);
inline int ble_gattc_disc_svc_by_uuid(uint16_t, const ble_uuid_t*, ble_gatt_disc_svc_fn*, void*){return 1;}
inline int ble_gattc_disc_all_chrs(uint16_t, uint16_t, uint16_t, ble_gatt_chr_fn*, void*){return 1;}
inline int ble_gattc_disc_all_dscs(uint16_t, uint16_t, uint16_t, ble_gatt_dsc_fn*, void*){return 1;}
inline int ble_gattc_write_flat(uint16_t, uint16_t, const void*, uint16_t, ble_gatt_attr_fn*, void*){return 1;}
inline int ble_gap_security_initiate(uint16_t){return 0;}
struct ble_hs_cfg_t { int sm_bonding, sm_mitm, sm_sc, sm_io_cap, sm_our_key_dist, sm_their_key_dist; };
extern ble_hs_cfg_t ble_hs_cfg;
