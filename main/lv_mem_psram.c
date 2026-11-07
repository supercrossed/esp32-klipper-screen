/*
 * LVGL allocator (CONFIG_LV_USE_CUSTOM_MALLOC): put every LVGL object, style
 * and decoded image in PSRAM. Without this, small LVGL allocations land in
 * internal SRAM (SPIRAM_MALLOC_ALWAYSINTERNAL), which Wi-Fi/SDIO and DMA need.
 */
#include "lvgl.h"
#include "src/stdlib/lv_mem.h"
#include "esp_heap_caps.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#define LV_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { LV_UNUSED(mem); LV_UNUSED(bytes); return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, LV_CAPS);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = heap_caps_realloc(p, new_size, LV_CAPS);
    return n ? n : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    lv_memzero(mon, sizeof(*mon));
    mon->free_size = heap_caps_get_free_size(LV_CAPS);
    mon->total_size = heap_caps_get_total_size(LV_CAPS);
    mon->free_biggest_size = heap_caps_get_largest_free_block(LV_CAPS);
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

#endif
