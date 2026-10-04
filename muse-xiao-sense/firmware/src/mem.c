#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_heap.h>
#include <mbedtls/platform.h>
#include "cJSON.h"
#include "mem.h"

LOG_MODULE_REGISTER(mem, LOG_LEVEL_INF);

static uint8_t s_mem[CONFIG_MUSE_HEAP_SIZE] __aligned(8);
static struct sys_heap s_heap;
static struct k_spinlock s_lock;

void *mem_alloc(size_t size)
{
	k_spinlock_key_t key = k_spin_lock(&s_lock);
	void *p = sys_heap_alloc(&s_heap, size);
	k_spin_unlock(&s_lock, key);
	if (!p && size) {
		LOG_ERR("out of memory (%u bytes)", (unsigned)size);
	}
	return p;
}

void *mem_calloc(size_t n, size_t size)
{
	if (size && n > SIZE_MAX / size) {
		return NULL;
	}
	void *p = mem_alloc(n * size);
	if (p) {
		memset(p, 0, n * size);
	}
	return p;
}

void mem_free(void *ptr)
{
	if (!ptr) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&s_lock);
	sys_heap_free(&s_heap, ptr);
	k_spin_unlock(&s_lock, key);
}

void mem_log(const char *where)
{
	struct sys_memory_stats st;
	k_spinlock_key_t key = k_spin_lock(&s_lock);
	sys_heap_runtime_stats_get(&s_heap, &st);
	k_spin_unlock(&s_lock, key);
	LOG_INF("heap %s: %u in use, peak %u of %u", where, (unsigned)st.allocated_bytes,
		(unsigned)st.max_allocated_bytes, (unsigned)CONFIG_MUSE_HEAP_SIZE);
}

static int mem_init(void)
{
	sys_heap_init(&s_heap, s_mem, sizeof(s_mem));
	cJSON_Hooks hooks = {.malloc_fn = mem_alloc, .free_fn = mem_free};
	cJSON_InitHooks(&hooks);
#if !defined(CONFIG_MBEDTLS_ENABLE_HEAP)
	mbedtls_platform_set_calloc_free(mem_calloc, mem_free);
#endif
	return 0;
}

/* Before mbedTLS and PSA initialise. */
SYS_INIT(mem_init, PRE_KERNEL_1, 0);
