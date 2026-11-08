/*
 * Keep esp_hosted's SDIO data buffers out of internal RAM.
 *
 * esp_hosted allocates every buffer the C6 link moves data through with
 * MEM_ALLOC = heap_caps_aligned_alloc(64, len, INTERNAL | DMA | 8BIT), and
 * asserts when that fails:
 *   - RX: in streaming mode each SDIO read pulls everything the C6 has queued
 *     into one buffer grown to fit. A burst (webcam, console history, the OTA
 *     download) asks for tens of KB in one contiguous block
 *     (assert in sdio_rx_get_buffer).
 *   - TX: each outgoing packet is copied into a 1536-byte pool block. Many
 *     connections at once (printer scan) need many blocks
 *     (assert copy_buff in transport_drv_sta_tx).
 * Internal RAM is small and fragments, so both restart the device.
 *
 * The ESP32-P4 SDMMC host can DMA to and from PSRAM (SOC_SDMMC_PSRAM_DMA_CAPABLE)
 * when the buffer and transfer are cache-line aligned, and esp_hosted moves
 * whole 512-byte blocks only (H_SDIO_RX/TX_BLOCK_ONLY_XFER), so these buffers
 * are served from PSRAM here; the SDMMC driver syncs the cache around each
 * transfer. Linked with -Wl,--wrap=heap_caps_aligned_alloc (main/CMakeLists.txt)
 * so it survives fresh downloads of the component.
 *
 * Only requests matching MEM_ALLOC exactly (64-byte alignment, INTERNAL | DMA |
 * 8BIT) of at least 1 KB are redirected. That covers the RX stream buffer and
 * the TX pool blocks: the pool asks for MEMPOOL_ALIGNED(1536), which is 1600
 * because the macro always adds 64 (so a "multiple of 512" test missed them and
 * a big upload, such as a screenshot, still ran internal RAM out). Smaller
 * esp_hosted allocations (register buffers) stay internal. Nothing else in this
 * firmware asks for these caps with 64-byte alignment (AES uses 1 without flash
 * encryption, SHA 16, regdma 4, display buffers have no DMA cap). The size is
 * rounded up to whole cache lines so cache syncs never touch a neighbour. If
 * PSRAM runs out the request falls back to internal RAM as before.
 */
#include <stddef.h>
#include <stdint.h>
#include "esp_heap_caps.h"

#define HOSTED_CAPS  (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
#define PSRAM_LINE   128   /* CONFIG_CACHE_L2_CACHE_LINE_SIZE; SDMMC needs this alignment in PSRAM */
#define MIN_SIZE     1024

void *__real_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);

/* shown by the `status` console command */
volatile uint32_t g_hosted_rx_psram_n, g_hosted_rx_psram_max;

void *__wrap_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
    if (alignment == 64 && caps == HOSTED_CAPS && size >= MIN_SIZE) {
        size_t lines = (size + PSRAM_LINE - 1) & ~(size_t)(PSRAM_LINE - 1);
        void *p = __real_heap_caps_aligned_alloc(PSRAM_LINE, lines, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) {
            g_hosted_rx_psram_n++;
            if (size > g_hosted_rx_psram_max) g_hosted_rx_psram_max = size;
            return p;
        }
    }
    return __real_heap_caps_aligned_alloc(alignment, size, caps);
}
