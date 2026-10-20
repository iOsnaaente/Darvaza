/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * A uClibc define __GLIBC__ (compatibilidade) mas não tem execinfo.h, e o
 * ggml o inclui quando vê __GLIBC__. Só serve para imprimir a pilha num
 * erro fatal: sem ela, o erro sai sem a pilha.
 */
#pragma once

static inline int backtrace(void **buffer, int size)
{
    (void)buffer;
    (void)size;
    return 0;
}

static inline void backtrace_symbols_fd(void *const *buffer, int size, int fd)
{
    (void)buffer;
    (void)size;
    (void)fd;
}
