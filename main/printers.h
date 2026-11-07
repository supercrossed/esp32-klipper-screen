#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MAX_PRINTERS   12
#define MAX_HEATERS    8
#define MAX_LANES      16
#define MAX_WEBCAMS    4
#define MAX_MACROS     48

typedef struct {
    char     name[32];
    char     host[64];
    uint16_t port;          /* Moonraker port, usually 7125 (or 80 via Mainsail/Fluidd nginx) */
    char     api_key[65];   /* optional */
} printer_cfg_t;

typedef enum {
    PS_OFFLINE = 0,   /* Moonraker unreachable */
    PS_KLIPPY_DOWN,   /* Moonraker up, Klipper not ready (startup/shutdown/error/disconnected) */
    PS_STANDBY,
    PS_PRINTING,
    PS_PAUSED,
    PS_COMPLETE,
    PS_CANCELLED,
    PS_ERROR,
} printer_status_t;

typedef struct {
    char  obj[48];      /* Klipper object name, e.g. "extruder", "heater_generic chamber" */
    char  label[20];    /* UI label */
    float temp;
    float target;
    float power;        /* 0..1, -1 if n/a */
    bool  settable;     /* has a target we can set */
    uint8_t kind;       /* 0 hotend, 1 bed, 2 chamber, 3 other */
} heater_t;

typedef enum { MMU_NONE = 0, MMU_AFC, MMU_HAPPY_HARE } mmu_type_t;

typedef struct {
    char     name[20];      /* lane / gate name */
    char     tool[8];       /* mapped tool, e.g. "T0" */
    char     material[16];
    char     spool[24];     /* filament name / spool id text */
    uint32_t color;         /* 0xRRGGBB */
    bool     has_color;
    bool     present;       /* filament present / available */
    bool     loaded;        /* this lane is loaded into the toolhead */
    bool     active;        /* selected / current */
    bool     at_hub;        /* AFC: filament reached the hub */
    int      weight_g;      /* -1 unknown */
    char     status[20];
} mmu_lane_t;

typedef struct {
    mmu_type_t type;
    int        nlanes;
    mmu_lane_t lanes[MAX_LANES];
    char       current[24];   /* current lane/gate/tool text */
    char       filament[24];  /* loaded state text */
    char       action[32];
    char       unit[32];      /* AFC unit names / HH model */
    int        active_lane;   /* index into lanes, -1 none */
    /* Where the active filament is, for the path graphic:
     * -1 unknown, 0 parked in lane/gate, 1 at hub/selector, 2 in bowden,
     *  3 in extruder, 4 loaded to nozzle */
    int        stage;
    char       hub_name[12];  /* "Hub" (AFC) / "Selector" (HH) */
} mmu_info_t;

typedef struct {
    char name[32];
    char snapshot_url[200];
    char stream_url[200];
    bool flip_h, flip_v;
    int  rotation;
} webcam_t;

typedef struct {
    /* connection */
    printer_status_t status;
    bool     online;
    char     klippy_state[16];
    char     message[160];        /* state_message / error text */
    char     hostname[48];
    char     sw_version[40];
    int64_t  last_ok_us;

    /* job */
    char     filename[160];
    float    progress;            /* 0..1 */
    float    print_duration;      /* s, printing only */
    float    total_duration;      /* s, incl. pauses */
    float    filament_used;       /* mm */
    float    est_slicer;          /* s, slicer estimate from metadata, 0 = unknown */
    float    eta_s;               /* remaining seconds (best estimate), -1 unknown */
    int      layer, layer_total;  /* -1 unknown */
    float    speed_factor, extrude_factor;

    /* machine */
    int      nheaters;
    heater_t heaters[MAX_HEATERS];
    float    pos[4];
    char     homed[8];
    float    fan_speed;           /* part fan 0..1, -1 if none */
    bool     idle_timeout_printing;

    mmu_info_t mmu;

    int      nwebcams;
    webcam_t webcams[MAX_WEBCAMS];

    /* capabilities found in /printer/objects/list */
    int      nmacros;
    char     macros[MAX_MACROS][40];   /* user macros (names starting with '_' hidden) */
    bool     has_qgl, has_ztilt, has_bed_mesh;
    bool     has_spoolman;             /* Moonraker [spoolman] configured */
    bool     spoolman_connected;
    int      active_spool;             /* Spoolman spool id, -1 none */

    /* thumbnail (PNG bytes, owned by the poller; copy under lock) */
    uint8_t *thumb;
    size_t   thumb_len;
    uint32_t thumb_ver;

    uint32_t seq;                 /* increments on every successful poll */
} printer_state_t;

/* ── registry ─────────────────────────────────────────────────────────── */
void printers_init(void);                  /* load from NVS, start pollers */
int  printers_count(void);
bool printers_get_cfg(int idx, printer_cfg_t *out);
int  printers_add(const printer_cfg_t *cfg);              /* returns index or -1 */
bool printers_update(int idx, const printer_cfg_t *cfg);
bool printers_remove(int idx);
int  printers_find_host(const char *host, uint16_t port); /* -1 if not found */

/* Copy current state (thumbnail pointer is NOT copied; use printers_copy_thumb). */
bool printers_get_state(int idx, printer_state_t *out);
/* Copy thumbnail if version differs from *ver. Returns malloc'd buffer (caller frees) or NULL. */
uint8_t *printers_copy_thumb(int idx, uint32_t *ver, size_t *len);

/* Which printer is in the foreground (polled fast). -1 = home screen. */
void printers_set_focus(int idx);

/* ── commands (asynchronous; executed on a worker task) ─────────────── */
typedef enum {
    CMD_GCODE = 0, CMD_PAUSE, CMD_RESUME, CMD_CANCEL, CMD_ESTOP,
    CMD_FIRMWARE_RESTART, CMD_KLIPPER_RESTART,
    /* Moonraker machine API; gcode = service name for the SERVICE_* commands */
    CMD_SERVICE_RESTART, CMD_SERVICE_STOP, CMD_SERVICE_START, CMD_HOST_REBOOT, CMD_HOST_SHUTDOWN,
} printer_cmd_t;

void printers_command(int idx, printer_cmd_t cmd, const char *gcode);
/* G-code with a friendly name for the result toast; quiet = only toast on failure. */
void printers_gcode(int idx, const char *gcode, const char *label, bool quiet);
/* Last command result text for the UI toast (returns true once per new result). */
bool printers_last_result(char *out, size_t cap);

const char *printer_status_str(printer_status_t s);
