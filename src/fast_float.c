/*
 * fast_float.c - C port of the fast_float library (decimal -> binary32)
 *
 * Faithfully reproduces the core of the C++ fast_float library:
 *   1. Clinger's fast path (exact for small mantissa / exponent)
 *   2. The Eisel-Lemire algorithm (compute_float), including the
 *      round-to-even and subnormal handling described in:
 *
 *      Noble Mushtak and Daniel Lemire,
 *      "Fast Number Parsing Without Fallback", (2020-present).
 *
 * Since the powers-of-five table is shared with the double range and spans
 * 5^-342 .. 5^308, it is kept in fast_float_tables.h (generated from the
 * C++ sources).  Only the float (binary32) specialization is implemented here,
 * which is what pgvector's vector_in() needs.
 *
 * For inputs with more than 19 significant digits (too_many_digits), the
 * truncated 64-bit mantissa would no longer be exact, so we fall back to the
 * correctly-rounded libc strtof().  This is provably identical to the result
 * the C++ big-integer slow path would have produced, and such inputs are
 * astronomically rare in practice, so the hot path remains allocation-free.
 */
#include "fast_float.h"
#include "fast_float_tables.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>			/* strtof */
#include <string.h>			/* memcpy */

/* ---- binary_format<float> constants ---- */
#define FF_MANTISSA_BITS       23
#define FF_MINIMUM_EXP         (-127)
#define FF_INFINITE_POWER      0xFF
#define FF_SIGN_INDEX          31
#define FF_MIN_EXP_FAST_PATH   (-10)
#define FF_MAX_EXP_FAST_PATH   10
#define FF_MIN_EXP_ROUND_EVEN  (-17)
#define FF_MAX_EXP_ROUND_EVEN  10
#define FF_SMALLEST_POW10      (-64)
#define FF_LARGEST_POW10       38
#define FF_MAX_MANTISSA_FAST   (0x1000000u)	/* 2 << mantissa_explicit_bits */

#define FF_OK         1		/* success */
#define FF_ERROR      0		/* invalid syntax          */
#define FF_OVERFLOW   (-1)	/* result is +-infinity    */

/* exact powers of ten used by Clinger's fast path (float) */
static const float ff_powers_of_ten[FF_MAX_EXP_FAST_PATH + 1] = {
	1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f
};

/* largest integer v so that (5^index * v) <= 1<<24 */
static const uint64_t ff_max_mantissa_fast_path[12] = {
	0x1000000,	/* 16777216      */
	3355443,
	671088,
	134217,
	26843,
	5368,
	1073,
	214,
	42,
	8,
	1,
	0
};

/* 128-bit product */
typedef struct ff_value128
{
	uint64_t	low;
	uint64_t	high;
} ff_value128;

typedef struct ff_adjusted_mantissa
{
	uint64_t	mantissa;
	int32_t		power2;
} ff_adjusted_mantissa;

static inline int
ff_leading_zeroes(uint64_t x)
{
	return __builtin_clzll(x);
}

/* a*b -> 128 bits */
static inline ff_value128
ff_full_multiplication(uint64_t a, uint64_t b)
{
	__uint128_t	r = (__uint128_t) a * b;

	return (ff_value128) { (uint64_t) r, (uint64_t) (r >> 64) };
}

/*
 * Approximate w * 5^q.  See fast_float compute_product_approximation.
 * index = 2 * (q - smallest_power_of_five).
 */
static inline ff_value128
ff_compute_product_approximation(int bit_precision, int64_t q, uint64_t w)
{
	int			index = 2 * ((int) q - FF_SMALLEST_POWER_OF_FIVE);
	ff_value128 firstproduct = ff_full_multiplication(w,
													 ff_power_of_five_128[index]);
	uint64_t	precision_mask =
		(bit_precision < 64) ? (0xFFFFFFFFFFFFFFFFULL >> bit_precision)
		: 0xFFFFFFFFFFFFFFFFULL;

	if ((firstproduct.high & precision_mask) == precision_mask)
	{
		ff_value128 secondproduct = ff_full_multiplication(w,
														   ff_power_of_five_128[index + 1]);

		firstproduct.low += secondproduct.high;
		if (secondproduct.high > firstproduct.low)
			firstproduct.high++;
	}
	return firstproduct;
}

/* floor(p) + q where p = log2(5^q) approximation; +63 */
static inline int32_t
ff_power(int32_t q)
{
	return (int32_t) ((((152170 + 65536) * (int64_t) q) >> 16) + 63);
}

/*
 * Eisel-Lemire: w * 10^q -> adjusted_mantissa.
 * Faithful C port of fast_float compute_float<binary_format<float>>.
 */
static inline ff_adjusted_mantissa
ff_compute_float(int64_t q, uint64_t w)
{
	ff_adjusted_mantissa answer;

	if (w == 0 || q < FF_SMALLEST_POW10)
		return (ff_adjusted_mantissa) { 0, 0 };

	if (q > FF_LARGEST_POW10)
		return (ff_adjusted_mantissa) { 0, FF_INFINITE_POWER };

	/* shift so that the most significant bit of w is 1 */
	int			lz = ff_leading_zeroes(w);

	w <<= lz;

	/* precision = mantissa_explicit_bits + 3 = 26 */
	ff_value128 product = ff_compute_product_approximation(FF_MANTISSA_BITS + 3,
														   q, w);

	int			upperbit = (int) (product.high >> 63);
	int			shift = upperbit + 64 - FF_MANTISSA_BITS - 3;

	answer.mantissa = product.high >> shift;
	answer.power2 = ff_power((int32_t) q) + upperbit - lz - FF_MINIMUM_EXP;

	if (answer.power2 <= 0)	/* subnormal? */
	{
		if (-answer.power2 + 1 >= 64)
			return (ff_adjusted_mantissa) { 0, 0 };	/* zero */

		answer.mantissa >>= -answer.power2 + 1;
		answer.mantissa += (answer.mantissa & 1);	/* round up */
		answer.mantissa >>= 1;
		answer.power2 = (answer.mantissa < (1u << FF_MANTISSA_BITS)) ? 0 : 1;
		return answer;
	}

	/* rarely: we may fall exactly between two floats */
	if (product.low <= 1 &&
		q >= FF_MIN_EXP_ROUND_EVEN && q <= FF_MAX_EXP_ROUND_EVEN &&
		(answer.mantissa & 3) == 1)
	{
		if ((answer.mantissa << shift) == product.high)
			answer.mantissa &= ~(uint64_t) 1;	/* do not round up */
	}

	answer.mantissa += (answer.mantissa & 1);	/* round up */
	answer.mantissa >>= 1;
	if (answer.mantissa >= (2u << FF_MANTISSA_BITS))
	{
		answer.mantissa = 1u << FF_MANTISSA_BITS;
		answer.power2++;
	}

	answer.mantissa &= ~(1u << FF_MANTISSA_BITS);
	if (answer.power2 >= FF_INFINITE_POWER)
		return (ff_adjusted_mantissa) { 0, FF_INFINITE_POWER };

	return answer;
}

/* pack adjusted_mantissa + sign into a float (to_float) */
static inline float
ff_to_float(bool negative, ff_adjusted_mantissa am)
{
	uint32_t	word = (uint32_t) am.mantissa |
		((uint32_t) am.power2 << FF_MANTISSA_BITS);
	float		result;

	if (negative)
		word |= (uint32_t) 1 << FF_SIGN_INDEX;
	memcpy(&result, &word, sizeof(result));
	return result;
}

/* Clinger's fast path (float). Returns true on success. */
static inline bool
ff_clinger_fast_path(uint64_t mantissa, int64_t exponent, bool negative,
					 float *value)
{
	if (FF_MIN_EXP_FAST_PATH <= exponent && exponent <= FF_MAX_EXP_FAST_PATH)
	{
		/* assume fegetround() == FE_TONEAREST (the default and the only mode
		 * produced by PG runtime) */
		if (mantissa <= FF_MAX_MANTISSA_FAST)
		{
			float		v = (float) mantissa;

			if (exponent < 0)
				v /= ff_powers_of_ten[-exponent];
			else
				v *= ff_powers_of_ten[exponent];
			if (negative)
				v = -v;
			*value = v;
			return true;
		}
	}
	return false;
}

int
fast_float_parse(const char *p, size_t len, float *value, const char **endp)
{
	const char *orig = p;
	const char *pend = p + len;
	bool		negative = false;

	if (p == pend)
		return FF_ERROR;

	if (*p == '+' || *p == '-')
	{
		negative = (*p == '-');
		p++;
	}

	uint64_t	m = 0;		/* mantissa (exact while sig <= 19) */
	int64_t		fpt = 0;	/* total fraction digit count   */
	bool		have_first_nonzero = false;
	int			sig = 0;	/* significant digit count      */
	int			overflow = 0;	/* sig > 19                     */

	/* ----- integer part ----- */
	while (p != pend && *p >= '0' && *p <= '9')
	{
		int			d = *p - '0';

		if (d != 0)
			have_first_nonzero = true;
		if (have_first_nonzero)
		{
			sig++;
			if (sig <= 19)
				m = m * 10 + (uint64_t) d;
			else
				overflow = 1;
		}
		p++;
	}

	/* ----- fraction part ----- */
	if (p != pend && *p == '.')
	{
		p++;
		while (p != pend && *p >= '0' && *p <= '9')
		{
			int			d = *p - '0';

			if (d != 0)
				have_first_nonzero = true;
			if (have_first_nonzero)
			{
				sig++;
				if (sig <= 19)
					m = m * 10 + (uint64_t) d;
				else
					overflow = 1;
			}
			fpt++;
			p++;
		}
	}

	/* need at least one digit somewhere */
	{
		const char *t = orig;

		if (*t == '+' || *t == '-')
			t++;
		{
			bool		saw_digit = false;

			while (t != pend)
			{
				if (*t >= '0' && *t <= '9')
				{
					saw_digit = true;
					break;
				}
				if (*t == '.')
				{
					t++;
					continue;
				}
				break;
			}
			if (!saw_digit)
				return FF_ERROR;
		}
	}

	/* ----- exponent part ----- */
	int64_t		exp = 0;

	if (p != pend && (*p == 'e' || *p == 'E'))
	{
		const char *save = p;

		p++;
		bool		neg_exp = false;

		if (p != pend && *p == '-')
		{
			neg_exp = true;
			p++;
		}
		else if (p != pend && *p == '+')
			p++;

		if (p == pend || !(*p >= '0' && *p <= '9'))
			p = save;		/* 'e' not part of the number: stop here */
		else
		{
			int64_t		e = 0;

			while (p != pend && *p >= '0' && *p <= '9')
			{
				if (e < 0x10000000)
					e = e * 10 + (*p - '0');
				p++;
			}
			exp = neg_exp ? -e : e;
		}
	}

	/* decimal exponent such that value = +-m * 10^q */
	int64_t		q = exp - fpt;

	*endp = p;

	if (overflow)
	{
		/* more than 19 significant digits: fall back to correctly-rounded
		 * libc conversion for exactness (identical to the C++ bigint slow
		 * path, and to the original strtof behaviour). */
		char	   *end;
		float		v;

		errno = 0;
		v = strtof(orig, &end);
		*endp = end;
		if (isinf(v) && m != 0)
		{
			/* Overflow: v is +-Inf.  Define *value like the fast-path
			 * overflow return below, so the FF_OVERFLOW contract is
			 * uniform regardless of which path produced it. */
			*value = v;
			return FF_OVERFLOW;
		}
		*value = v;
		return FF_OK;
	}

	/* fast paths */
	if (ff_clinger_fast_path(m, q, negative, value))
		return FF_OK;

	ff_adjusted_mantissa am = ff_compute_float(q, m);

	if (am.power2 == FF_INFINITE_POWER)
	{
		*value = negative ? -INFINITY : INFINITY;
		return FF_OVERFLOW;
	}

	*value = ff_to_float(negative, am);
	return FF_OK;
}