#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SPOOL_MAX 160

/* kinds (also the Spoolman page tabs) */
enum { SM_SPOOL = 0, SM_FILAMENT = 1, SM_VENDOR = 2, SM_KINDS = 3 };

/* Form page argument (PAGE_SPOOL_FORM) */
enum { FM_NEW = 0, FM_EDIT = 1, FM_COPY = 2, FM_SPOOL_OF = 3 };
#define SPOOL_FORM_ARG(kind, mode, id) ((kind) | ((mode) << 2) | ((id) << 4))

typedef struct {
    int      id;
    char     name[40];       /* filament name (spools, filaments) / vendor name */
    char     vendor[28];
    char     material[16];
    uint32_t color;          /* 0xRRGGBB (first colour of a multi-colour filament) */
    bool     has_color;
    float    remaining_g;    /* -1 unknown */
    float    total_g;        /* net filament weight, -1 unknown (vendors: empty spool weight) */
    float    spool_g;        /* empty spool weight, -1 unknown */
    char     location[28];
    char     lot[24];
    int      ext_temp, bed_temp;   /* filament settings, 0 unknown */
    int      filament_id, vendor_id;   /* -1 none */
    float    density, diameter;
    bool     archived;
} spool_item_t;

typedef enum { SM_IDLE = 0, SM_LOADING, SM_OK, SM_ERROR } sm_state_t;

/* Fetch spools, filaments, vendors and the form metadata via the printer's Moonraker proxy (async). */
void        spoolman_fetch(int printer_idx);
sm_state_t  spoolman_state(char *err, int cap);
uint32_t    spoolman_version(void);          /* bumps when new data lands */
int         spoolman_active_spool(void);     /* printer's active spool id, -1 none */
int         spoolman_printer(void);          /* printer whose Moonraker is used */

/* Copy out (UI thread). Returns count. Spools include archived ones (item.archived). */
int  spoolman_items(int kind, spool_item_t *out, int max);
bool spoolman_item(int kind, int id, spool_item_t *out);

/* Set / clear (id < 0) the printer's active spool (async). */
void spoolman_set_active(int printer_idx, int spool_id);
void spoolman_free(void);                    /* release cached data when the Spoolman pages close */
/* The list page is opening a form: keep the data when the list page closes. */
void spoolman_hold(void);
bool spoolman_held(void);                    /* true once (clears the hold) */

/* ── form metadata (filled by spoolman_fetch) ─────────────────────────── */
typedef enum { XF_TEXT, XF_INT, XF_INT_RANGE, XF_FLOAT, XF_FLOAT_RANGE, XF_DATETIME, XF_BOOL, XF_CHOICE } xf_type_t;
#define SM_XF_MAX 12
typedef struct {
    char      key[32];
    char      name[40];
    char      unit[12];
    xf_type_t type;
    bool      multi;          /* choice: several may be picked */
    char      deflt[64];      /* JSON-encoded default value, "" none */
    char      choices[256];   /* choice: '\n'-separated */
} sm_xfield_t;

typedef struct {
    char material[24];
    float density;
    int ext_temp, bed_temp;   /* 0 unknown */
} sm_material_t;

int  spoolman_xfields(int kind, sm_xfield_t *out, int max);
int  spoolman_locations(char (*out)[64], int max);
int  spoolman_materials(sm_material_t *out, int max);   /* used materials first, then Spoolman's list */
void spoolman_currency(char *out, int cap);              /* symbol, e.g. "€" */
void spoolman_web_url(char *out, int cap);               /* Spoolman's web UI root, "" unknown */

/* ── writes / single reads ────────────────────────────────────────────── */
/* One request at a time through the proxy: method "GET"/"POST"/"PATCH"/"PUT"/"DELETE",
 * path like "/v1/spool", body a JSON text or NULL. repeat > 1 sends it that many
 * times (several identical spools). Returns false if one is already running. */
bool        spoolman_job(const char *method, const char *path, const char *body, int repeat);
/* SM_LOADING while running, then SM_OK / SM_ERROR. msg gets the error. id = "id" of
 * the (last) returned object, -1 none. */
sm_state_t  spoolman_job_state(char *msg, int cap, int *id);
/* JSON text of the last job's response (malloc'd, caller frees), NULL none. */
char       *spoolman_job_response(void);
void        spoolman_job_clear(void);

/* After a save: the list page selects this item when it comes back. */
void spoolman_set_focus(int kind, int id);
bool spoolman_take_focus(int *kind, int *id);

/* ── Spoolman's external filament database (SpoolmanDB) ───────────────── */
typedef struct {
    char  ext_id[96];
    char  manufacturer[40];
    char  name[64];
    char  material[24];
    float density, diameter, weight, spool_weight;   /* -1 unknown */
    char  color_hex[8];
    char  color_hexes[64];    /* comma-separated, multi-colour */
    char  direction[16];      /* coaxial / longitudinal */
    int   ext_temp, bed_temp; /* 0 unknown */
} sm_ext_t;

void        spoolman_ext_load(void);              /* download once (async, ~3.5 MB in PSRAM) */
sm_state_t  spoolman_ext_state(int *kbytes);
/* Every word of q must appear in manufacturer, name, material or diameter. */
int         spoolman_ext_search(const char *q, sm_ext_t *out, int max, int *total);
void        spoolman_ext_free(void);
