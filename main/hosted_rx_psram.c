/*
 * Keep esp_hosted's big SDIO receive buffer out of internal RAM.
 *
 * The C6 link runs in streaming mode: each SDIO read pulls everything the C6
 * has queued into one buffer, which esp_hosted (re)allocates to fit with
 * heap_caps_aligned_alloc(64, len, INTERNAL | DMA | 8BIT) and asserts on
 * failure. A burst (webcam, console history, the OTA download) can ask for
 * tens of KB in one contiguous block; with internal RAM fragmented that fails
 * and the device restarts (assert in sdio_rx_get_buffer).
 *
 * The ESP32-P4 SDMMC host can DMA straight to PSRAM (SOC_SDMMC_PSRAM_DMA_CAPABLE)
 * when the buffer is cache-line aligned, and esp_hosted reads whole 512-byte
 * blocks only (H_SDIO_RX_BLOCK_ONLY_XFER), so those large requests are served
 * from PSRAM here. Linked with -Wl,--wrap=heap_caps_aligned_alloc (see
 * main/CMakeLists.txt) so it survives fresh downloads of the component.
 *
 * Only requests that match esp_hosted's MEM_ALLOC exactly (64-byte alignment,
 * INTERNAL | DMA | 8BIT), are at least 4 KB and a whole number of 512-byte
 * blocks are redirected. esp_hosted's
 * other buffers (mempool blocks, ~1.6 KB) and everything else pass through.
 */
#include <stddef.h>
#include <stdint.h>
#include "esp_heap_caps.h"

#define HOSTED_CAPS   (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
#define PSRAM_LINE    128   /* CONFIG_CACHE_L2_CACHE_LINE_SIZE; SDMMC needs this alignment in PSRAM */
#define MIN_REDIRECT  4096

void *__real_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);

/* shown by the `status` console command */
volatile uint32_t g_hosted_rx_psram_n, g_hosted_rx_psram_max;

void *__wrap_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
    /* esp_hosted's stream buffer: MEM_ALLOC caps, >= 4 KB, whole 512-byte SDIO blocks */
    if (alignment == 64 && caps == HOSTED_CAPS && size >= MIN_REDIRECT && size % 512 == 0) {
        size_t sz = (size + PSRAM_LINE - 1) & ~(size_t)(PSRAM_LINE - 1);
        void *p = __real_heap_caps_aligned_alloc(PSRAM_LINE, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) {
            g_hosted_rx_psram_n++;
            if (sz > g_hosted_rx_psram_max) g_hosted_rx_psram_max = sz;
            return p;
        }
    }
    return __real_heap_caps_aligned_alloc(alignment, size, caps);
}
