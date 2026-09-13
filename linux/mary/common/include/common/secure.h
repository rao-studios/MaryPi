/* For secrets — the Mistral key above all: wiping memory the compiler may not
 * optimize away, and comparing without leaking where two values differ. */
#ifndef MARY_COMMON_SECURE_H
#define MARY_COMMON_SECURE_H

#include <stdbool.h>
#include <stddef.h>

void mc_secure_zero(void *p, size_t n);
/* Constant-time over n bytes. */
bool mc_secure_equal(const void *a, const void *b, size_t n);

#endif
