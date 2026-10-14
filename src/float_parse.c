/*
 * float_parse.c - Shared decimal-to-float parser for vector/halfvec text input.
 *
 * The parser modes mirror the ones previously duplicated between vector.c and
 * halfvec.c:
 *   strtof       -> libc strtof (locale/errno/NaN-Inf overhead)
 *   fast_float   -> fast_float_parse() C port (Eisel-Lemire)
 *   inline       -> hand-written inline decimal-to-float parser
 *
 * The type input functions pass their own type name ("vector" / "halfvec")
 * so the error messages remain specific to the type being parsed.
 */
#include "postgres.h"

#include <math.h>
#include <string.h>

#include "fast_float.h"
#include "float_parse.h"
#include "port.h"				/* for strtof() */

const struct config_enum_entry float_parse_mode_options[] = {
	{"strtof", FLOAT_PARSE_STRTOF, false},
	{"inline", FLOAT_PARSE_INLINE, false},
	{"fast_float", FLOAT_PARSE_FAST_FLOAT, false},
	{NULL, 0, false}
};

/* Single shared mode for vector_in() and halfvec_in(). */
int			float_parse_mode = FLOAT_PARSE_STRTOF;

/*
 * Parse a single float value from *pt using the given runtime parse mode.
 * Returns a pointer to just past the parsed value.
 */
char *
float_parse_float(int mode, char *pt, float *val, const char *lit, const char *type_name)
{
	switch (mode)
	{
		case FLOAT_PARSE_INLINE:
			{
				float		sign = 1.0f;

				if (*pt == '-') { sign = -1.0f; pt++; }
				else if (*pt == '+') { pt++; }

				uint64		int_part = 0;

				while (*pt >= '0' && *pt <= '9')
					int_part = int_part * 10 + (*pt++ - '0');

				uint64		frac_part = 0;
				uint64		frac_div = 1;

				if (*pt == '.')
				{
					pt++;
					while (*pt >= '0' && *pt <= '9' && frac_div <= 100000000ULL)
					{
						frac_part = frac_part * 10 + (*pt++ - '0');
						frac_div *= 10;
					}
					while (*pt >= '0' && *pt <= '9')
						pt++;
				}

				*val = (float) (int64) int_part + (float) frac_part / (float) frac_div;

				if (*pt == 'e' || *pt == 'E')
				{
					pt++;
					int			exp_sign = 1;

					if (*pt == '-') { exp_sign = -1; pt++; }
					else if (*pt == '+') { pt++; }

					int			exp_val = 0;

					while (*pt >= '0' && *pt <= '9')
						exp_val = exp_val * 10 + (*pt++ - '0');

					if (exp_sign > 0)
						for (int j = 0; j < exp_val; j++) *val *= 10.0f;
					else
						for (int j = 0; j < exp_val; j++) *val *= 0.1f;
				}

				*val *= sign;
			}
			return pt;

		case FLOAT_PARSE_FAST_FLOAT:
			{
				const char *fend;
				size_t		flen = strcspn(pt, ",]");
				int			rc = fast_float_parse(pt, flen, val, &fend);

				if (rc == FF_ERROR || fend == pt)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("invalid input syntax for type %s: \"%s\"", type_name, lit)));
				if (rc == FF_OVERFLOW)
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("\"%s\" is out of range for type %s",
									pnstrdup(pt, fend - pt), type_name)));
				return (char *) fend;
			}

		default:				/* strtof */
			{
				char	   *stringEnd;

				errno = 0;
				*val = strtof(pt, &stringEnd);
				if (stringEnd == pt)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("invalid input syntax for type %s: \"%s\"", type_name, lit)));
				if (errno == ERANGE && isinf(*val))
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("\"%s\" is out of range for type %s",
									pnstrdup(pt, stringEnd - pt), type_name)));
				return stringEnd;
			}
	}
}

/*
 * Human-readable name of a parse mode, for logging.
 */
const char *
float_parse_mode_str(int mode)
{
	switch (mode)
	{
		case FLOAT_PARSE_INLINE:
			return "inline";
		case FLOAT_PARSE_FAST_FLOAT:
			return "fast_float";
		default:
			return "strtof";
	}
}