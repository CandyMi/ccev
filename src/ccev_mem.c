/**
 * @file ccev_mem.c
 * @brief Memory allocator replacement — default + public setter.
 *
 * @author CandyMi
 * @license MIT
 *
 * The function pointers ccev__realloc_fn and ccev__free_fn are the
 * sole allocation path used by every ccev subsystem (including the
 * intrusive data structures from ccalg, which route through
 * CCHEAP_REALLOC / CCHASHMAP_REALLOC macros defined in
 * ccev_internal.h).
 *
 * Users who need a custom allocator (e.g. a pool, jemalloc, or
 * memory tracking) must call ccev_set_allocator() BEFORE any
 * ccev_loop_create() call.
 */

#include "ccev_internal.h"

/* ── Default allocator (libc realloc/free) ────────────────── */

static void *ccev_default_realloc(void *ptr, size_t sz) {
    if (sz == 0) { free(ptr); return NULL; }
    return realloc(ptr, sz);
}
static void ccev_default_free(void *ptr) { free(ptr); }

/* ── Global function pointers (extern'd in ccev_internal.h) ── */

void *(*ccev__realloc_fn)(void*, size_t) = ccev_default_realloc;
void  (*ccev__free_fn)(void*)            = ccev_default_free;

/* ── Public API ────────────────────────────────────────────── */

void ccev_set_allocator(void *(*realloc_fn)(void*, size_t),
                        void  (*free_fn)(void*)) {
    if (realloc_fn) ccev__realloc_fn = realloc_fn;
    if (free_fn)    ccev__free_fn    = free_fn;
}
