// Copied from github.com/AbdoWise-z/uconnect-vdd include/uconnect-vdd.h (protocol 1.0).
// Keep identical to the driver release packaging pins; see docs/third-party-screen.md.
#pragma once
/*
 * uConnect virtual display driver: its control interface.
 *
 * Shared by the driver (MSVC) and its clients (MSVC or MinGW), so it is plain
 * C with fixed-size fields: the structures have the same layout under both.
 *
 * A client opens the device interface below and adds monitors through it.
 * Every monitor belongs to the handle that added it:
 *   - closing the handle (or the process ending, however it ends) removes it;
 *   - a handle that sends nothing for UCVDD_WATCHDOG_MS loses its monitors,
 *     so a hung client cannot leave a screen nobody is looking at.
 * Clients send IOCTL_UCVDD_PING about once a second while they own monitors.
 */

#include <windows.h>
#include <winioctl.h>
#include <stdint.h>

#define UCVDD_PROTOCOL_MAJOR 1
#define UCVDD_PROTOCOL_MINOR 0

/* Hardware ID of the root-enumerated device the installer creates. */
#define UCVDD_HARDWARE_ID L"Root\\uConnectVDD"

/* {e9003ec9-0502-462d-81a2-48e52c886607} */
static const GUID UCVDD_INTERFACE_GUID =
    {0xe9003ec9, 0x0502, 0x462d, {0x81, 0xa2, 0x48, 0xe5, 0x2c, 0x88, 0x66, 0x07}};

#define UCVDD_WATCHDOG_MS 3000

/* Limits the driver enforces on a monitor's preferred mode. */
#define UCVDD_MIN_WIDTH 640
#define UCVDD_MAX_WIDTH 7680
#define UCVDD_MIN_HEIGHT 480
#define UCVDD_MAX_HEIGHT 4320
#define UCVDD_MIN_REFRESH_MHZ 24000
#define UCVDD_MAX_REFRESH_MHZ 240000
#define UCVDD_MAX_MONITORS_PER_HANDLE 4

#define UCVDD_IOCTL(n) CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900 + (n), METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Out: UCVDD_VERSION. */
#define IOCTL_UCVDD_GET_VERSION UCVDD_IOCTL(0)
/* In: UCVDD_ADD_PARAMS. Out: UCVDD_ADD_RESULT. Adding an id this handle
 * already owns returns that monitor again; an id another handle owns fails
 * with ERROR_ALREADY_EXISTS. */
#define IOCTL_UCVDD_ADD UCVDD_IOCTL(1)
/* In: UCVDD_REMOVE_PARAMS. Only this handle's own monitors. */
#define IOCTL_UCVDD_REMOVE UCVDD_IOCTL(2)
/* Keeps this handle's monitors alive; no buffers. */
#define IOCTL_UCVDD_PING UCVDD_IOCTL(3)
/* In: UCVDD_RENDER_ADAPTER_PARAMS. The GPU that renders monitors added
 * afterwards; set it to the encoder's GPU so frames stay on one adapter. */
#define IOCTL_UCVDD_SET_RENDER_ADAPTER UCVDD_IOCTL(4)

typedef struct UCVDD_VERSION {
    uint32_t Major;
    uint32_t Minor;
    uint32_t Patch;
    uint32_t WatchdogMs;
    uint32_t MaxMonitors;  /* across all handles */
} UCVDD_VERSION;

typedef struct UCVDD_ADD_PARAMS {
    /* Stable per display device, so the OS remembers its arrangement and
     * scale; also its EDID serial number. */
    GUID MonitorId;
    uint32_t Width;
    uint32_t Height;
    uint32_t RefreshMilliHz;   /* 60000 = 60 Hz, 59940 = 59.94 Hz */
    char Name[14];             /* EDID monitor name, at most 13 ASCII characters */
    char Serial[14];           /* EDID serial string, at most 13 ASCII characters */
} UCVDD_ADD_PARAMS;

typedef struct UCVDD_ADD_RESULT {
    LUID AdapterLuid;          /* with TargetId, finds the monitor in QueryDisplayConfig */
    uint32_t TargetId;
} UCVDD_ADD_RESULT;

typedef struct UCVDD_REMOVE_PARAMS {
    GUID MonitorId;
} UCVDD_REMOVE_PARAMS;

typedef struct UCVDD_RENDER_ADAPTER_PARAMS {
    LUID AdapterLuid;
} UCVDD_RENDER_ADAPTER_PARAMS;
