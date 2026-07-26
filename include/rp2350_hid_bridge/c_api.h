#pragma once

#include <stdint.h>

#if defined(_WIN32) && defined(RP2350_HID_BRIDGE_BUILD_DLL)
#define RP2350_HID_API __declspec(dllexport)
#elif defined(_WIN32)
#define RP2350_HID_API __declspec(dllimport)
#else
#define RP2350_HID_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RP2350_HID_ABI_MAJOR 1u
#define RP2350_HID_ABI_MINOR 0u
#define RP2350_HID_FEATURE_SHARED_SESSION UINT64_C(1)
#define RP2350_HID_FEATURE_PORT_DISCOVERY (UINT64_C(1) << 1)

typedef struct Rp2350HidSession Rp2350HidSession;

typedef enum Rp2350HidStatus {
    RP2350_HID_STATUS_TIMEOUT = -2,
    RP2350_HID_STATUS_ERROR = -1,
    RP2350_HID_STATUS_OK = 0,
    RP2350_HID_STATUS_FOUND = 1
} Rp2350HidStatus;

typedef struct Rp2350HidAbiInfo {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t options_size;
    uint64_t feature_flags;
} Rp2350HidAbiInfo;

typedef struct Rp2350HidOptions {
    uint32_t struct_size;
    const char* port;
    uint32_t baud;
    uint32_t timeout_ms;
    int32_t retries;
    uint32_t heartbeat_interval_ms;
} Rp2350HidOptions;

RP2350_HID_API int32_t rp2350_hid_get_abi_info(Rp2350HidAbiInfo* info);
RP2350_HID_API const char* rp2350_hid_last_error(void);
RP2350_HID_API int32_t rp2350_hid_find_port(
    uint16_t vid,
    uint16_t pid,
    char* output,
    uint32_t output_size
);

RP2350_HID_API int32_t rp2350_hid_session_create(
    const Rp2350HidOptions* options,
    Rp2350HidSession** session
);
RP2350_HID_API int32_t rp2350_hid_session_retain(Rp2350HidSession* session);
RP2350_HID_API void rp2350_hid_session_release(Rp2350HidSession* session);
RP2350_HID_API int32_t rp2350_hid_session_open(Rp2350HidSession* session);
RP2350_HID_API int32_t rp2350_hid_session_is_open(
    Rp2350HidSession* session,
    int32_t* is_open
);
RP2350_HID_API int32_t rp2350_hid_session_ping(Rp2350HidSession* session);
RP2350_HID_API int32_t rp2350_hid_session_info(
    Rp2350HidSession* session,
    uint8_t* output,
    uint32_t output_size,
    uint32_t* bytes_written
);
RP2350_HID_API int32_t rp2350_hid_session_caps(
    Rp2350HidSession* session,
    uint8_t* output,
    uint32_t output_size,
    uint32_t* bytes_written
);
RP2350_HID_API int32_t rp2350_hid_session_type_text(
    Rp2350HidSession* session,
    const char* text
);
RP2350_HID_API int32_t rp2350_hid_session_key_tap(
    Rp2350HidSession* session,
    const char* combo
);
RP2350_HID_API int32_t rp2350_hid_session_key_down(
    Rp2350HidSession* session,
    const char* combo
);
RP2350_HID_API int32_t rp2350_hid_session_key_up(
    Rp2350HidSession* session,
    const char* combo
);
RP2350_HID_API int32_t rp2350_hid_session_mouse_move(
    Rp2350HidSession* session,
    int16_t dx,
    int16_t dy
);
RP2350_HID_API int32_t rp2350_hid_session_mouse_click(
    Rp2350HidSession* session,
    const char* button
);
RP2350_HID_API int32_t rp2350_hid_session_mouse_down(
    Rp2350HidSession* session,
    const char* button
);
RP2350_HID_API int32_t rp2350_hid_session_mouse_up(
    Rp2350HidSession* session,
    const char* button
);
RP2350_HID_API int32_t rp2350_hid_session_mouse_wheel(
    Rp2350HidSession* session,
    int8_t delta
);
RP2350_HID_API int32_t rp2350_hid_session_wait_ms(
    Rp2350HidSession* session,
    uint32_t milliseconds
);
RP2350_HID_API int32_t rp2350_hid_session_stop_all(Rp2350HidSession* session);
RP2350_HID_API int32_t rp2350_hid_session_run_script(
    Rp2350HidSession* session,
    const char* script
);

#ifdef __cplusplus
}
#endif
