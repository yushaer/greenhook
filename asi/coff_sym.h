/* Fast PE COFF symbol table resolver for unstripped MinGW/LLVM PE binaries. */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    void *addr;
} CoffSymReq;

/* Resolves all named symbols in `mod` into `reqs[k].addr`. Returns count resolved. */
size_t coff_resolve_batch(void *hmodule, CoffSymReq *reqs, size_t count);

/* Convenience helper to resolve a single symbol from `mod`. */
void *coff_resolve_one(void *hmodule, const char *name);

#ifdef __cplusplus
}
#endif
