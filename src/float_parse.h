/*
 * float_parse.h - Shared decimal-to-float parser for text input types.
 *
 * Both vector_in() and halfvec_in() need to turn a decimal floating-point
 * literal into a float.  They also both support the same set of runtime
 * parse modes (GUC): strtof, inline, fast_float.  This single implementation
 * is shared between the two type input functions; only the type name used in
 * error messages differs.
 */
#ifndef FLOAT_PARSE_H
#define FLOAT_PARSE_H

#include <stddef.h>

#include "utils/guc.h"		/* for config_enum_entry */

typedef enum FloatParseMode
{
	FLOAT_PARSE_STRTOF = 0,
	FLOAT_PARSE_INLINE = 1,
	FLOAT_PARSE_FAST_FLOAT = 2
} FloatParseMode;

extern const struct config_enum_entry float_parse_mode_options[];
extern int	float_parse_mode;

/*
 * Parse a single float value from *pt using the given parse mode.
 *
 * Returns a pointer to just past the parsed value.  On error, raises an ERROR
 * whose message names 'type_name' (e.g. "vector" or "halfvec").
 */
extern char *float_parse_float(int mode, char *pt, float *val, const char *lit, const char *type_name);

/* Human-readable name of a parse mode, for logging. */
extern const char *float_parse_mode_str(int mode);

#endif /* FLOAT_PARSE_H */