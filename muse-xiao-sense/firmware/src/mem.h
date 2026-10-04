/* The gadget's own heap: cJSON, mbedTLS, the Noise session buffers and setup
 * messages all draw from one fixed budget (CONFIG_MUSE_HEAP_SIZE). */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *mem_alloc(size_t size);
void *mem_calloc(size_t n, size_t size);
void mem_free(void *ptr);
/* Logs current and peak use. */
void mem_log(const char *where);

#ifdef __cplusplus
}
#endif
