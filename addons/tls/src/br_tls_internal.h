/* br_tls_internal.h - tls internals shared with the generated CA bundle and the test. */
#ifndef BR_TLS_INTERNAL_H
#define BR_TLS_INTERNAL_H

#include "braty/br_tls.h"

#include <stddef.h>

/* The Mozilla CA bundle (addons/tls/certs/cacert.pem) as PEM text + NUL, made at configure time
 * (br_tls_cacert.c in the build directory). br_tls_cacert_size excludes the NUL. */
extern const unsigned char br_tls_cacert[];
extern const size_t br_tls_cacert_size;

/* Root certificates parsed from the built-in bundle: -1 while the parse job runs (this starts it
 * when nothing has), else the count (0 = the bundle did not parse). For the test. */
int br_tls_builtin_root_count(br_engine *e);

#endif
