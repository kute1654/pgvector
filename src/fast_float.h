/*
 * fast_float.h - C port of the fast_float library
 *
 * Provides a correctly-rounded decimal-to-binary32 (float) conversion using
 * Clinger's fast path and the Eisel-Lemire algorithm, faithful to the C++
 * fast_float library.  This header exposes only the parser entry point used
 * by pgvector's vector_in().
 */
#ifndef FAST_FLOAT_H
#define FAST_FLOAT_H

#include <stddef.h>

/* return codes from fast_float_parse() */
#define FF_OK         1		/* success                    */
#define FF_ERROR      0		/* invalid syntax             */
#define FF_OVERFLOW   (-1)	/* result is +-infinity        */

/*
 * Parse a decimal floating-point literal.
 *
 * On success returns 1 and stores the parsed value and one-past-last consumed
 * character in *endp.  On failure returns 0 (invalid) or -1 (out of range).
 *
 * Contract on *value:
 *   - FF_OK: *value is the correctly-rounded parsed float.
 *   - FF_OVERFLOW: *value is +-Infinity (the sign of the overflowed literal);
 *     FF_ERROR: *value is left undefined.  *endp is always set.
 */
int fast_float_parse(const char *p, size_t len, float *value, const char **endp);

#endif /* FAST_FLOAT_H */