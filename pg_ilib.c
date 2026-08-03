/*
 * pg_ilib — typed binary serialization for PostgreSQL
 *
 * Encodes PostgreSQL values into a compact bytea with a 2-byte header:
 *
 *   Byte 0: [ op_id (4 bits) | params_hi (4 bits) ]
 *   Byte 1: [ params_lo (8 bits) ]
 *   Bytes 2…N: payload
 *
 * op_id table:
 *   0x01  text                params = unused
 *   0x02  numeric / bigint    params = decimal scale
 *   0x03  bool                params = 0
 *   0x04  timestamp / date    params = signed tz offset in minutes
 *   0x08  uuid                params = 0
 *   0x0E  jsonb               params = unused
 *   0x0F  hex                 params = unused
 *
 * pg_ilib_check_header() validates every (op_id, params, payload_size)
 * combination before any decode logic runs, so garbage input raises
 * ERRCODE_DATA_CORRUPTED instead of crashing the server.
 */
#include "postgres.h"
#include "utils/int8.h"
#include "utils/builtins.h"
#include "utils/uuid.h"
#include "utils/json.h"
#include "utils/numeric.h"
#include "utils/jsonb.h"
#include "utils/bytea.h"
#include "utils/timestamp.h"
#include "utils/formatting.h"
#include "libpq/pqformat.h"

#include "catalog/pg_collation.h"

#include <gmp.h>  /* GNU Multiple Precision — required for numeric/bigint */

#define HEXBASE         256
#define bufsize         32
#define ADDRESS_LEN     20

#ifdef PG_MODULE_MAGIC
    PG_MODULE_MAGIC;
#endif

#if PG_VERSION_NUM < 110000
#define DatumGetJsonbP(d)  ((Jsonb *) PG_DETOAST_DATUM(d))
#define JsonbPGetDatum(p)  PointerGetDatum(p)
#define PG_GETARG_JSONB_P(x)   DatumGetJsonbP(PG_GETARG_DATUM(x))
#define PG_RETURN_JSONB_P(x)   PG_RETURN_POINTER(x)
#endif
        
/* NUMERIC */ 
typedef int16 NumericDigit;

struct NumericShort
{
        uint16          n_header;               /* Sign + display scale + weight */
        NumericDigit n_data[FLEXIBLE_ARRAY_MEMBER]; /* Digits */
};

struct NumericLong
{
        uint16          n_sign_dscale;  /* Sign + display scale */
        int16           n_weight;               /* Weight of 1st digit  */
        NumericDigit n_data[FLEXIBLE_ARRAY_MEMBER]; /* Digits */
};

union NumericChoice
{
        uint16          n_header;               /* Header word */
        struct NumericLong n_long;      /* Long form (4-byte header) */
        struct NumericShort n_short;    /* Short form (2-byte header) */
};       
struct NumericData
{
        int32           vl_len_;                /* varlena header (do not touch directly!) */
        union NumericChoice choice; /* choice of format */
};

#define MASK     	0xF0

#define NUMERIC_NAN                                             0xC000
#define DEC_DIGITS      4

#define NUMERIC_HDRSZ   (VARHDRSZ + sizeof(uint16) + sizeof(int16))
#define NUMERIC_HDRSZ_SHORT (VARHDRSZ + sizeof(uint16))

#define NUMERIC_SHORT_WEIGHT_SIGN_MASK  0x0040
#define NUMERIC_SHORT_WEIGHT_MASK               0x003F
#define NUMERIC_SHORT_SIGN_MASK                 0x2000
#define NUMERIC_SHORT_DSCALE_MASK               0x1F80
#define NUMERIC_SHORT_DSCALE_SHIFT              7

#define NUMERIC_SHORT_WEIGHT_MAX                NUMERIC_SHORT_WEIGHT_MASK
#define NUMERIC_SHORT_WEIGHT_MIN                (-(NUMERIC_SHORT_WEIGHT_MASK+1))
#define NUMERIC_SHORT_DSCALE_MAX                \
        (NUMERIC_SHORT_DSCALE_MASK >> NUMERIC_SHORT_DSCALE_SHIFT)
        
#define NUMERIC_POS                     0x0000
#define NUMERIC_NEG                     0x4000
#define NUMERIC_PINF                    0xD000
#define NUMERIC_NINF                    0xF000

#define NUMERIC_SHORT           0x8000
#define NUMERIC_SPECIAL         0xC000
#define NUMERIC_SIGN_MASK       0xC000
#define NUMERIC_DSCALE_MASK                     0x3FFF
#define NUMERIC_EXT_SIGN_MASK   0xF000

#define NUMERIC_FLAGBITS(n) ((n)->choice.n_header & NUMERIC_SIGN_MASK)
#define NUMERIC_EXT_FLAGBITS(n) ((n)->choice.n_header & NUMERIC_EXT_SIGN_MASK)
#define NUMERIC_HEADER_IS_SHORT(n)      (((n)->choice.n_header & 0x8000) != 0)
#define NUMERIC_IS_SPECIAL(n)   (NUMERIC_FLAGBITS(n) == NUMERIC_SPECIAL)
#define NUMERIC_IS_SHORT(n)             (NUMERIC_FLAGBITS(n) == NUMERIC_SHORT)

#define NUMERIC_CAN_BE_SHORT(scale,weight) \
        ((scale) <= NUMERIC_SHORT_DSCALE_MAX && \
        (weight) <= NUMERIC_SHORT_WEIGHT_MAX && \
        (weight) >= NUMERIC_SHORT_WEIGHT_MIN) 

#define NUMERIC_DIGITS(num) (NUMERIC_HEADER_IS_SHORT(num) ? \
        (num)->choice.n_short.n_data : (num)->choice.n_long.n_data)
        
#define NUMERIC_HEADER_SIZE(n) \
        (VARHDRSZ + sizeof(uint16) + \
         (NUMERIC_HEADER_IS_SHORT(n) ? 0 : sizeof(int16)))
         
#define NUMERIC_NDIGITS(num) \
        ((VARSIZE(num) - NUMERIC_HEADER_SIZE(num)) / sizeof(NumericDigit))
        
#define NUMERIC_SIGN(n) \
        (NUMERIC_IS_SHORT(n) ? \
                (((n)->choice.n_short.n_header & NUMERIC_SHORT_SIGN_MASK) ? \
                 NUMERIC_NEG : NUMERIC_POS) : \
                (NUMERIC_IS_SPECIAL(n) ? \
                 NUMERIC_EXT_FLAGBITS(n) : NUMERIC_FLAGBITS(n)))

#define NUMERIC_DSCALE(n)       (NUMERIC_HEADER_IS_SHORT((n)) ? \
        ((n)->choice.n_short.n_header & NUMERIC_SHORT_DSCALE_MASK) \
                >> NUMERIC_SHORT_DSCALE_SHIFT \
        : ((n)->choice.n_long.n_sign_dscale & NUMERIC_DSCALE_MASK))

#define NUMERIC_WEIGHT(n)       (NUMERIC_HEADER_IS_SHORT((n)) ? \
        (((n)->choice.n_short.n_header & NUMERIC_SHORT_WEIGHT_SIGN_MASK ? \
                ~NUMERIC_SHORT_WEIGHT_MASK : 0) \
        | ((n)->choice.n_short.n_header & NUMERIC_SHORT_WEIGHT_MASK)) \
        : ((n)->choice.n_long.n_weight))

#define init_var(v)             memset(v, 0, sizeof(NumericVar))

#define digitbuf_alloc(ndigits)  \
        ((NumericDigit *) palloc((ndigits) * sizeof(NumericDigit)))
#define digitbuf_free(buf)      \
        do { \
                 if ((buf) != NULL) \
                         pfree(buf); \
        } while (0)

typedef struct NumericVar
{
        int                     ndigits;                /* # of digits in digits[] - can be 0! */
        int                     weight;                 /* weight of first digit */
        int                     sign;                   /* NUMERIC_POS, _NEG, _NAN, _PINF, or _NINF */
        int                     dscale;                 /* display scale */
        NumericDigit *buf;                      /* start of palloc'd space for digits[] */
        NumericDigit *digits;           /* base-NBASE digits */
} NumericVar;

static void init_var_from_num(Numeric num, NumericVar *dest);
char       *pgtypes_alloc(long);

char *
pgtypes_alloc(long size)
{
        char       *new = (char *) calloc(1L, size);

        if (!new)
                errno = ENOMEM;
        return new;
}

/*
 * make_result_opt_error() -
 *
 *	Create the packed db numeric format in palloc()'d memory from
 *	a variable.  This will handle NaN and Infinity cases.
 *
 *	If "have_error" isn't NULL, on overflow *have_error is set to true and
 *	NULL is returned.  This is helpful when caller needs to handle errors.
 */
static Numeric
make_result_opt_error(const NumericVar *var, bool *have_error)
{
    Numeric		result;
    NumericDigit *digits = var->digits;
    int			weight = var->weight;
    int			sign = var->sign;
    int			n;
    Size		len;

    if (have_error)
	*have_error = false;

    if ((sign & NUMERIC_SIGN_MASK) == NUMERIC_SPECIAL)
    {
	/*
	 * Verify valid special value.  This could be just an Assert, perhaps,
	 * but it seems worthwhile to expend a few cycles to ensure that we
	 * never write any nonzero reserved bits to disk.
	 */
	if (!(sign == NUMERIC_NAN ||
	      sign == NUMERIC_PINF ||
	      sign == NUMERIC_NINF))
	    elog(ERROR, "invalid numeric sign value 0x%x", sign);

	result = (Numeric) palloc(NUMERIC_HDRSZ_SHORT);

	SET_VARSIZE(result, NUMERIC_HDRSZ_SHORT);
	result->choice.n_header = sign;
	/* the header word is all we need */
	return result;
    }

    n = var->ndigits;

    /* truncate leading zeroes */
    while (n > 0 && *digits == 0)
    {
	digits++;
	weight--;
	n--;
    }
    /* truncate trailing zeroes */
    while (n > 0 && digits[n - 1] == 0)
	n--;

    /* If zero result, force to weight=0 and positive sign */
    if (n == 0)
    {
	weight = 0;
	sign = NUMERIC_POS;
    }

    /* Build the result */
    if (NUMERIC_CAN_BE_SHORT(var->dscale, weight))
    {
	len = NUMERIC_HDRSZ_SHORT + n * sizeof(NumericDigit);
	result = (Numeric) palloc(len);
	SET_VARSIZE(result, len);
	result->choice.n_short.n_header =
	    (sign == NUMERIC_NEG ? (NUMERIC_SHORT | NUMERIC_SHORT_SIGN_MASK)
	     : NUMERIC_SHORT)
	    | (var->dscale << NUMERIC_SHORT_DSCALE_SHIFT)
	    | (weight < 0 ? NUMERIC_SHORT_WEIGHT_SIGN_MASK : 0)
	    | (weight & NUMERIC_SHORT_WEIGHT_MASK);
    }
    else
    {
	len = NUMERIC_HDRSZ + n * sizeof(NumericDigit);
	result = (Numeric) palloc(len);
	SET_VARSIZE(result, len);
	result->choice.n_long.n_sign_dscale =
	    sign | (var->dscale & NUMERIC_DSCALE_MASK);
	result->choice.n_long.n_weight = weight;
    }

    Assert(NUMERIC_NDIGITS(result) == n);
    if (n > 0)
	memcpy(NUMERIC_DIGITS(result), digits, n * sizeof(NumericDigit));

    /* Check for overflow of int16 fields */
    if (NUMERIC_WEIGHT(result) != weight ||
	NUMERIC_DSCALE(result) != var->dscale)
    {
	if (have_error)
	{
	    *have_error = true;
	    return NULL;
	}
	else
	{
	    ereport(ERROR,
		    (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
		     errmsg("value overflows numeric format")));
	}
    }
    return result;
}

/*
 * make_result() -
 *
 *      An interface to make_result_opt_error() without "have_error" argument.
 */
static Numeric
make_result(const NumericVar *var)
{
        return make_result_opt_error(var, NULL);               
}

/*
 * alloc_var() -
 *
 *      Allocate a digit buffer of ndigits digits (plus a spare digit for rounding)
 */
static void
alloc_var(NumericVar *var, int ndigits)
{
        digitbuf_free(var->buf);
        var->buf = digitbuf_alloc(ndigits + 1);
        var->buf[0] = 0;                        /* spare digit for rounding */
        var->digits = var->buf + 1;
        var->ndigits = ndigits;
}

/*
 * free_var() -
 *
 *      Return the digit buffer of a variable to the free pool
 */
static void
free_var(NumericVar *var)
{
        digitbuf_free(var->buf);
        var->buf = NULL;
        var->digits = NULL;
        var->sign = NUMERIC_NAN;
}

/*
 * strip_var
 *              
 * Strip any leading and trailing zeroes from a numeric variable
 */
static void     
strip_var(NumericVar *var)
{
        NumericDigit *digits = var->digits;
        int                     ndigits = var->ndigits;

        /* Strip leading zeroes */
        while (ndigits > 0 && *digits == 0)
        {
                digits++;
                var->weight--;
                ndigits--;
        }
        
        /* Strip trailing zeroes */
        while (ndigits > 0 && digits[ndigits - 1] == 0)
                ndigits--;

        /* If it's zero, normalize the sign and weight */
        if (ndigits == 0)
        {
                var->sign = NUMERIC_POS;
                var->weight = 0;
        }
        
        var->digits = digits;
        var->ndigits = ndigits;
}

/*
 * set_var_from_str()
 *
 *	Parse a string and put the number into a variable
 *
 * This function does not handle leading or trailing spaces.  It returns
 * the end+1 position parsed, so that caller can check for trailing
 * spaces/garbage if deemed necessary.
 *
 * cp is the place to actually start parsing; str is what to use in error
 * reports.  (Typically cp would be the same except advanced over spaces.)
 */
static const char *
set_var_from_str(const char *str, const char *cp, NumericVar *dest)
{
    bool		have_dp = false;
    int			i;
    unsigned char *decdigits;
    int			sign = NUMERIC_POS;
    int			dweight = -1;
    int			ddigits;
    int			dscale = 0;
    int			weight;
    int			ndigits;
    int			offset;
    NumericDigit *digits;

    /*
     * We first parse the string to extract decimal digits and determine the
     * correct decimal weight.  Then convert to NBASE representation.
     */
    switch (*cp)
    {
	case '+':
	    sign = NUMERIC_POS;
	    cp++;
	    break;

	case '-':
	    sign = NUMERIC_NEG;
	    cp++;
	    break;
    }

    if (*cp == '.')
    {
	have_dp = true;
	cp++;
    }

    if (!isdigit((unsigned char) *cp))
	ereport(ERROR,
		(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
		 errmsg("invalid input syntax for type %s: \"%s\"",
			"numeric", str)));

    decdigits = (unsigned char *) palloc(strlen(cp) + DEC_DIGITS * 2);

    /* leading padding for digit alignment later */
    memset(decdigits, 0, DEC_DIGITS);
    i = DEC_DIGITS;

    while (*cp)
    {
	if (isdigit((unsigned char) *cp))
	{
	    decdigits[i++] = *cp++ - '0';
	    if (!have_dp)
		dweight++;
	    else
		dscale++;
	}
	else if (*cp == '.')
	{
	    if (have_dp)
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
				"numeric", str)));
	    have_dp = true;
	    cp++;
	}
	else
	    break;
    }

    ddigits = i - DEC_DIGITS;
    /* trailing padding for digit alignment later */
    memset(decdigits + i, 0, DEC_DIGITS - 1);

    /* Handle exponent, if any */
    if (*cp == 'e' || *cp == 'E')
    {
	long		exponent;
	char	   *endptr;

	cp++;
	exponent = strtol(cp, &endptr, 10);
	if (endptr == cp)
	    ereport(ERROR,
		    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
		     errmsg("invalid input syntax for type %s: \"%s\"",
			    "numeric", str)));
	cp = endptr;

	/*
	 * At this point, dweight and dscale can't be more than about
	 * INT_MAX/2 due to the MaxAllocSize limit on string length, so
	 * constraining the exponent similarly should be enough to prevent
	 * integer overflow in this function.  If the value is too large to
	 * fit in storage format, make_result() will complain about it later;
	 * for consistency use the same ereport errcode/text as make_result().
	 */
	if (exponent >= INT_MAX / 2 || exponent <= -(INT_MAX / 2))
	    ereport(ERROR,
		    (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
		     errmsg("value overflows numeric format")));
	dweight += (int) exponent;
	dscale -= (int) exponent;
	if (dscale < 0)
	    dscale = 0;
    }

    /*
     * Okay, convert pure-decimal representation to base NBASE.  First we need
     * to determine the converted weight and ndigits.  offset is the number of
     * decimal zeroes to insert before the first given digit to have a
     * correctly aligned first NBASE digit.
     */
    if (dweight >= 0)
	weight = (dweight + 1 + DEC_DIGITS - 1) / DEC_DIGITS - 1;
    else
	weight = -((-dweight - 1) / DEC_DIGITS + 1);
    offset = (weight + 1) * DEC_DIGITS - (dweight + 1);
    ndigits = (ddigits + offset + DEC_DIGITS - 1) / DEC_DIGITS;

    alloc_var(dest, ndigits);
    dest->sign = sign;
    dest->weight = weight;
    dest->dscale = dscale;

    i = DEC_DIGITS - offset;
    digits = dest->digits;

    while (ndigits-- > 0)
    {
#if DEC_DIGITS == 4
	*digits++ = ((decdigits[i] * 10 + decdigits[i + 1]) * 10 +
		     decdigits[i + 2]) * 10 + decdigits[i + 3];
#elif DEC_DIGITS == 2
	*digits++ = decdigits[i] * 10 + decdigits[i + 1];
#elif DEC_DIGITS == 1
	*digits++ = decdigits[i];
#else
#error unsupported NBASE
#endif
	i += DEC_DIGITS;
    }

    pfree(decdigits);

    /* Strip any leading/trailing zeroes, and normalize weight if zero */
    strip_var(dest);

    /* Return end+1 position for caller */
    return cp;
}

/*
 * get_str_from_var() -
 *
 *	Convert a var to text representation (guts of numeric_out).
 *	The var is displayed to the number of digits indicated by its dscale.
 *	Returns a palloc'd string.
 */
static char *
get_str_from_var(const NumericVar *var)
{
    int			dscale;
    char	   *str;
    char	   *cp;
    char	   *endcp;
    int			i;
    int			d;
    NumericDigit dig;

#if DEC_DIGITS > 1
    NumericDigit d1;
#endif

    dscale = var->dscale;

    /*
     * Allocate space for the result.
     *
     * i is set to the # of decimal digits before decimal point. dscale is the
     * # of decimal digits we will print after decimal point. We may generate
     * as many as DEC_DIGITS-1 excess digits at the end, and in addition we
     * need room for sign, decimal point, null terminator.
     */
    i = (var->weight + 1) * DEC_DIGITS;
    if (i <= 0)
	i = 1;

    str = palloc(i + dscale + DEC_DIGITS + 2);
    cp = str;

    /*
     * Output a dash for negative values
     */
    if (var->sign == NUMERIC_NEG)
	*cp++ = '-';

    /*
     * Output all digits before the decimal point
     */
    if (var->weight < 0)
    {
	d = var->weight + 1;
	*cp++ = '0';
    }
    else
    {
	for (d = 0; d <= var->weight; d++)
	{
	    dig = (d < var->ndigits) ? var->digits[d] : 0;
	    /* In the first digit, suppress extra leading decimal zeroes */
#if DEC_DIGITS == 4
	    {
		bool		putit = (d > 0);

		d1 = dig / 1000;
		dig -= d1 * 1000;
		putit |= (d1 > 0);
		if (putit)
		    *cp++ = d1 + '0';
		d1 = dig / 100;
		dig -= d1 * 100;
		putit |= (d1 > 0);
		if (putit)
		    *cp++ = d1 + '0';
		d1 = dig / 10;
		dig -= d1 * 10;
		putit |= (d1 > 0);
		if (putit)
		    *cp++ = d1 + '0';
		*cp++ = dig + '0';
	    }
#elif DEC_DIGITS == 2
	    d1 = dig / 10;
	    dig -= d1 * 10;
	    if (d1 > 0 || d > 0)
		*cp++ = d1 + '0';
	    *cp++ = dig + '0';
#elif DEC_DIGITS == 1
	    *cp++ = dig + '0';
#else
#error unsupported NBASE
#endif
	}
    }

    /*
     * If requested, output a decimal point and all the digits that follow it.
     * We initially put out a multiple of DEC_DIGITS digits, then truncate if
     * needed.
     */
    if (dscale > 0)
    {
	*cp++ = '.';
	endcp = cp + dscale;
	for (i = 0; i < dscale; d++, i += DEC_DIGITS)
	{
	    dig = (d >= 0 && d < var->ndigits) ? var->digits[d] : 0;
#if DEC_DIGITS == 4
	    d1 = dig / 1000;
	    dig -= d1 * 1000;
	    *cp++ = d1 + '0';
	    d1 = dig / 100;
	    dig -= d1 * 100;
	    *cp++ = d1 + '0';
	    d1 = dig / 10;
	    dig -= d1 * 10;
	    *cp++ = d1 + '0';
	    *cp++ = dig + '0';
#elif DEC_DIGITS == 2
	    d1 = dig / 10;
	    dig -= d1 * 10;
	    *cp++ = d1 + '0';
	    *cp++ = dig + '0';
#elif DEC_DIGITS == 1
	    *cp++ = dig + '0';
#else
#error unsupported NBASE
#endif
	}
	cp = endcp;
    }

    /*
     * terminate the string and return it
     */
    *cp = '\0';
    return str;
}

static void
init_var_from_num(Numeric num, NumericVar *dest)
{
        dest->ndigits = NUMERIC_NDIGITS(num);
        dest->weight = NUMERIC_WEIGHT(num);
        dest->sign = NUMERIC_SIGN(num);
        dest->dscale = NUMERIC_DSCALE(num);
        dest->digits = NUMERIC_DIGITS(num);
        dest->buf = NULL;                       /* digits array is not palloc'd */
}

/* -----------------------------------------------------------------------
 * pg_ilib_check_header — header sanity checker
 *
 * Validates that the combination of op_id + params is physically possible
 * given the actual bytea payload size.  Impossible combinations indicate
 * corrupted / garbage input and must be rejected BEFORE any decode logic
 * runs — otherwise we risk buffer overflows, OOM, or silent bad results.
 *
 * Call this right after extracting op_id and params from the 2-byte header.
 * On failure it raises a SQL ERROR (ERRCODE_DATA_CORRUPTED); it never
 * returns normally in the error case.
 * ----------------------------------------------------------------------- */
static void
pg_ilib_check_header(const char *caller,
                     unsigned char op_id,
                     short         params,
                     int           byte_count)  /* VARSIZE_ANY_EXHDR result */
{
    int payload = byte_count - 2;   /* bytes after the 2-byte header */

    /* Universal: must have room for the 2-byte header */
    if (byte_count < 2)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("pg_ilib %s: bytea too short (%d bytes, minimum is 2)",
                        caller, byte_count)));

    switch (op_id)
    {
        case 0x01: /* text — params unused, payload may be 0 (empty string) */
            /* nothing to check */
            break;

        case 0x02: /* numeric / bigint — params = decimal scale */
        {
            if (params < 0)
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("pg_ilib %s: numeric scale %d is negative",
                                caller, (int)params)));

            /*
             * A 0-byte payload is not corruption: GMP's mpz_export() returns
             * count=0 for the value 0, so numeric_to_bytea() encodes numeric
             * 0 as header-only (see pg_ilib.c numeric_to_bytea). mpz_import()
             * on 0 bytes correctly yields 0 in bytea_to_numeric/bytea_to_bigint,
             * and 0 is valid at any scale, so skip the max-scale check below.
             */
            if (payload > 0)
            {
                /*
                 * Each payload byte encodes at most ⌈log₁₀(256)⌉ = 3 decimal
                 * digits.  A scale larger than 3 × payload_bytes is
                 * mathematically impossible for the binary data present.
                 */
                int max_scale = payload * 3;

                if (params > max_scale)
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("pg_ilib %s: numeric scale %d is impossible for "
                                    "%d payload byte(s) (max scale = %d)",
                                    caller, (int)params, payload, max_scale)));
            }
            break;
        }

        case 0x03: /* bool — exactly 1 payload byte, params must be 0 */
            if (payload != 1)
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("pg_ilib %s: bool payload must be 1 byte, got %d",
                                caller, payload)));
            if (params != 0)
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("pg_ilib %s: bool params must be 0, got %d",
                                caller, (int)params)));
            break;

        case 0x04: /* timestamp — exactly 4 payload bytes, params = tz minutes */
            if (payload != 4)
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("pg_ilib %s: timestamp payload must be 4 bytes, got %d",
                                caller, payload)));
            /*
             * IANA tz offsets span UTC-12:00 … UTC+14:00
             * → [-720, 840] minutes.  Give a small extra margin for
             * half-hour zones and future additions: [-840, 840].
             */
            if ((short)params < -840 || (short)params > 840)
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("pg_ilib %s: timezone offset %d min is out of "
                                "valid range [-840, 840]",
                                caller, (int)(short)params)));
            break;

        case 0x08: /* uuid — exactly 16 payload bytes, params must be 0 */
            if (payload != 16)
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("pg_ilib %s: uuid payload must be 16 bytes, got %d",
                                caller, payload)));
            if (params != 0)
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("pg_ilib %s: uuid params must be 0, got %d",
                                caller, (int)params)));
            break;

        case 0x0E: /* jsonb — variable, at least 1 payload byte */
            if (payload < 1)
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("pg_ilib %s: jsonb payload is empty", caller)));
            break;

        case 0x0F: /* hex — variable, payload may be 0 (empty hex string) */
            /* nothing further to check */
            break;

        default:
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("pg_ilib %s: unknown op_id 0x%02x",
                            caller, (unsigned)op_id)));
            break;
    }
}

PG_FUNCTION_INFO_V1(bytea_to_bigint);
PG_FUNCTION_INFO_V1(bigint_to_bytea);

PG_FUNCTION_INFO_V1(bytea_to_str);
PG_FUNCTION_INFO_V1(str_to_bytea);

PG_FUNCTION_INFO_V1(hex_to_bytea);
PG_FUNCTION_INFO_V1(bytea_to_hex);

PG_FUNCTION_INFO_V1(date_to_bytea);
PG_FUNCTION_INFO_V1(bytea_to_date);

PG_FUNCTION_INFO_V1(bytea_to_uuid);
PG_FUNCTION_INFO_V1(uuid_to_bytea);

PG_FUNCTION_INFO_V1(bool_to_bytea);
PG_FUNCTION_INFO_V1(bytea_to_bool);

PG_FUNCTION_INFO_V1(timestamp_to_bytea);
PG_FUNCTION_INFO_V1(bytea_to_timestamp);
PG_FUNCTION_INFO_V1(bytea_to_timestamptz);

PG_FUNCTION_INFO_V1(jsonb_to_bytea);
PG_FUNCTION_INFO_V1(bytea_to_jsonb);

PG_FUNCTION_INFO_V1(cmp_date);

PG_FUNCTION_INFO_V1(value_to_jsonb);

PG_FUNCTION_INFO_V1(numeric_to_bytea);

PG_FUNCTION_INFO_V1(bytea_to_numeric);

Datum
bytea_to_bigint(PG_FUNCTION_ARGS);

Datum
bigint_to_bytea(PG_FUNCTION_ARGS);

Datum
bytea_to_str(PG_FUNCTION_ARGS);

Datum
str_to_bytea(PG_FUNCTION_ARGS);

Datum
bytea_to_hex(PG_FUNCTION_ARGS);

Datum
hex_to_bytea(PG_FUNCTION_ARGS);

Datum
bytea_to_hex(PG_FUNCTION_ARGS);

Datum
date_to_bytea(PG_FUNCTION_ARGS);

Datum
bytea_to_uuid(PG_FUNCTION_ARGS);

Datum
uuid_to_bytea(PG_FUNCTION_ARGS);

Datum
bytea_to_bool(PG_FUNCTION_ARGS);

Datum
bool_to_bytea(PG_FUNCTION_ARGS);

Datum
bytea_to_timestamp(PG_FUNCTION_ARGS);

Datum
bytea_to_timestamptz(PG_FUNCTION_ARGS);

Datum
timestamp_to_bytea(PG_FUNCTION_ARGS);

Datum
bytea_to_jsonb(PG_FUNCTION_ARGS);

Datum
jsonb_to_bytea(PG_FUNCTION_ARGS);

Datum
cmp_date(PG_FUNCTION_ARGS);

Datum
value_to_jsonb(PG_FUNCTION_ARGS);

Datum
numeric_to_bytea(PG_FUNCTION_ARGS);

Datum
bytea_to_numeric(PG_FUNCTION_ARGS);

text *cstring_to_text(const char *s);
extern char *text_to_cstring(const text *t);

typedef struct JsonbInState
{
    JsonbParseState *parseState;
    JsonbValue *res;
} JsonbInState;

typedef union char_int64{
    unsigned char    buf[32];
    int64  out;
}char_int64;

typedef union char_int32{
    unsigned char    buf[32];
    int32  out;
}char_int32;

/*
 * Grabbed from varlena.c. This is a private struct, and we shouldn't really be
 * using it. See comments in bytea_to_int for why it's necessary.
 */

Datum
bytea_to_bigint(PG_FUNCTION_ARGS)
{
    bytea       *inbytea;
    int         byte_count;
    unsigned char       bbuf[10];
    unsigned char       *b;
    unsigned char	op_id;
    char_int64 char_int;

    inbytea = PG_DETOAST_DATUM(PG_GETARG_BYTEA_P(0));
    byte_count = VARSIZE_ANY_EXHDR(inbytea);
    if(byte_count==0)
	PG_RETURN_NULL();
    if (byte_count > 10)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("pg_ilib bytea_to_bigint: input too large (%d bytes, max 10)",
                        byte_count)));

    memcpy(bbuf, VARDATA_ANY(inbytea),byte_count);
    byte_count-=2;
    op_id = (bbuf[0]&MASK)>>4;
    if(op_id == 0x02){
        short params_bi = ((bbuf[0] & 0x0F) << 8) | bbuf[1];
        pg_ilib_check_header("bytea_to_bigint", op_id, params_bi,
                             VARSIZE_ANY_EXHDR(inbytea));
        b = &bbuf[2];
        memset(char_int.buf,'\0',32);
        while(byte_count--)
            char_int.buf[byte_count] = *b++;
        PG_RETURN_INT64(char_int.out);
    }else{
        PG_RETURN_NULL();
    }
}
Datum
numeric_to_bytea(PG_FUNCTION_ARGS)
{
    Numeric         num = PG_GETARG_NUMERIC(0);
    int32           scale = PG_GETARG_INT32(1);
    NumericVar      x;
    bytea           *result;
    char            *digits;
    char            *clean_digits;
    void            *binary_data;
    unsigned char   *byte_array;
    size_t          count;
    mpz_t           a;
    short           params;

    init_var_from_num(num, &x);
    digits = get_str_from_var(&x);
    
    /* strip the decimal point: GMP needs a pure integer string */
    clean_digits = (char *)palloc(strlen(digits) + 1);
    char *src = digits, *dst = clean_digits;
    while (*src) {
        if (*src != '.') *dst++ = *src;
        src++;
    }
    *dst = '\0';
    
    /* convert decimal-integer string → GMP → big-endian bytes */
    mpz_init(a);
    mpz_set_str(a, clean_digits, 10);
    binary_data = mpz_export(NULL, &count, 1, sizeof(char), 0, 0, a);
    byte_array = (unsigned char *)binary_data;
    
    params = (short)scale;
    
    /* allocate: 2-byte header + GMP payload */
    result = (bytea *)palloc(VARHDRSZ + 2 + count);
    SET_VARSIZE(result, VARHDRSZ + 2 + count);
    
    unsigned char *data = (unsigned char *)VARDATA(result);
    data[0] = (0x02 << 4) | ((params >> 8) & 0x0F);
    data[1] = params & 0xFF;
    memcpy(data + 2, byte_array, count);
    
    free(binary_data);
    mpz_clear(a);
    pfree(clean_digits);
    pfree(digits);
    
    PG_RETURN_BYTEA_P(result);
}

Datum
bytea_to_numeric(PG_FUNCTION_ARGS)
{
    bytea       *inbytea;
    int         byte_count;
    NumericVar  result;
    Numeric     res;
    unsigned char *str;
    unsigned char *bbuf;
    unsigned char op_id;
    short params = 0;
    char *binary_str;
    mpz_t num;

    inbytea = PG_DETOAST_DATUM(PG_GETARG_BYTEA_P(0));
    byte_count = VARSIZE_ANY_EXHDR(inbytea);

    bbuf = (unsigned char *) palloc(VARHDRSZ + byte_count);
    mpz_init(num);
    memcpy(bbuf, VARDATA_ANY(inbytea),byte_count);
    byte_count-=2;
    op_id = (bbuf[0]&MASK)>>4;
    params = ((bbuf[0] & 0x0F) << 8) | bbuf[1];

    if(op_id != 0x02){
        PG_RETURN_NULL();
        ereport(ERROR,
            (errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
             errmsg("Input of %d bytes has be max 8 bytes (64 bits) in size", (int)VARSIZE_ANY_EXHDR(inbytea) )));
    }

    pg_ilib_check_header("bytea_to_numeric", op_id, params,
                         VARSIZE_ANY_EXHDR(inbytea));

    bbuf+=2;
    str = bbuf;
    /* reconstruct integer from big-endian payload bytes via GMP */
    /* import big-endian payload bytes into GMP */
    mpz_import(num, byte_count, 1, sizeof(str[0]), 0, 0, str);

    binary_str = mpz_get_str(NULL, 10, num);

    /* insert decimal point at the correct position */
    init_var(&result);

    if (params > 0) {
        size_t len = strlen(binary_str);
        char *decimal_str;

        if (params >= len) {
                /* fewer digits than scale: e.g. 123 with scale=5 → "0.00123" */
                int zeros_needed = params - len;
                decimal_str = palloc(3 + zeros_needed + len);
                strcpy(decimal_str, "0.");
                for (int i = 0; i < zeros_needed; i++) {
                decimal_str[2 + i] = '0';
                }
                strcpy(decimal_str + 2 + zeros_needed, binary_str);
        } else {
                /* insert decimal point inside: e.g. 12345 scale=2 → "123.45" */
                decimal_str = palloc(len + 2);
                size_t int_part_len = len - params;
                memcpy(decimal_str, binary_str, int_part_len);
                decimal_str[int_part_len] = '.';
                memcpy(decimal_str + int_part_len + 1, binary_str + int_part_len, params);
                decimal_str[len + 1] = '\0';
        }

        (void) set_var_from_str(decimal_str, decimal_str, &result);
        pfree(decimal_str);
    } else {
        /* params == 0: integer — return as-is */
        (void) set_var_from_str(binary_str, binary_str, &result);
    }
    res = make_result(&result);

    free_var(&result);
    PG_RETURN_NUMERIC(res);
}

Datum
bigint_to_bytea(PG_FUNCTION_ARGS)
{
    bytea           *outbytea;
    unsigned char   *data;
    int             byte_count;
    int             i;

    uint64      value = (uint64)PG_GETARG_INT64(0);
    short       params = (short)PG_GETARG_INT32(1);

    if (value > 0xffffffffffffff)
        ereport(ERROR,
                (errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
                 errmsg("pg_ilib bigint_to_bytea: value %lu exceeds max 7 magnitude bytes (56 bits)",
                        (unsigned long) value)));

    /* magnitude byte count, matching numeric_to_bytea's GMP export (0 bytes for value 0) */
    if(value>0xffffffffffff){
        byte_count = 7;
    }else if(value>0xffffffff){
        byte_count = 6;
    }else if(value>0xffffff){
        byte_count = 5;
    }else if(value>0xffff){
        byte_count = 3;
    }else if(value>0xff){
        byte_count = 2;
    }else if(value>0){
        byte_count = 1;
    }else{
        byte_count = 0;
    }

    /* allocate: 2-byte header (matching numeric_to_bytea) + magnitude payload */
    outbytea = (bytea*)palloc(VARHDRSZ + 2 + byte_count);
    SET_VARSIZE(outbytea, VARHDRSZ + 2 + byte_count);

    data = (unsigned char *)VARDATA(outbytea);
    data[0] = (0x02 << 4) | ((params >> 8) & 0x0F);
    data[1] = params & 0xFF;

    /* write magnitude bytes big-endian */
    for (i = byte_count - 1; i >= 0; i--)
    {
        data[2 + i] = value & 0xFF;
        value >>= 8;
    }

    PG_RETURN_BYTEA_P(outbytea);
}

Datum
bytea_to_str(PG_FUNCTION_ARGS)
{
    bytea       *inbytea;
    int         byte_count;
    char        *res;
    int         i;    unsigned char *buf;
    unsigned char op_id=0;
    short params = 0;
    pg_uuid_t  *uuid;
    mpz_t big_number;
    
    static const char hex_chars[] = "0123456789abcdef";
    StringInfoData sbuf;    
    inbytea = PG_DETOAST_DATUM(PG_GETARG_BYTEA_P(0));
    byte_count = VARSIZE_ANY_EXHDR(inbytea);
    buf = (unsigned char *) palloc(VARHDRSZ + byte_count);
    mpz_init(big_number);

    i =0;
    res = (char *) palloc(byte_count + VARHDRSZ);
    SET_VARSIZE(res, byte_count + VARHDRSZ);

    /* Always copy at least the 2-byte header so op_id/params are readable.
     * For text (op_id=0x01) the payload is read directly from inbytea below,
     * so copying only the header here is enough even for long strings. */
    if (byte_count <= 8)
        memcpy(buf, VARDATA_ANY(inbytea), byte_count);
    else
        memcpy(buf, VARDATA_ANY(inbytea), 2); /* header only */

    byte_count-=2;
    op_id = (buf[0]&MASK)>>4;
    params = ((buf[0] & 0x0F) << 8) | buf[1];

    pg_ilib_check_header("bytea_to_str", op_id, params, byte_count + 2);

    switch(op_id){
    case 0x01: /* text: data starts at byte 2 */
        {
            /* byte_count already reflects -=2, so it equals text length.
             * Access data directly from the detoasted varlena — the partial
             * buf copy above would be wrong for strings longer than 6 bytes. */
            int text_len = byte_count;
            const char *text_src = (const char *) VARDATA_ANY(inbytea) + 2;
            PG_RETURN_TEXT_P(cstring_to_text_with_len(text_src, text_len));
        }
        break;
    case 0x02://bigint        memcpy(buf, VARDATA_ANY(inbytea),byte_count);        byte_count--;        mpz_import(big_number, byte_count,1,sizeof(buf[0]),0, 0, &buf[2]);
        mpz_get_str(res, 10, big_number);
	    mpz_clear(big_number);
        PG_RETURN_TEXT_P(cstring_to_text(res));        break;
    case 0x03: /* bool */
        if(buf[2])
            PG_RETURN_TEXT_P(cstring_to_text("true"));
        else
            PG_RETURN_TEXT_P(cstring_to_text("false"));
        break;
    case 0x04: /* timestamp / date */
        {
            unsigned char *date_buf;
            Timestamp date_res;
            char_int32 date_char_int;
            char *date_b;
            int date_byte_count;

            if (byte_count > 5)
            {
                PG_RETURN_NULL();
                ereport(ERROR,
                    (errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
                     errmsg("Input of %d bytes has be max 5 bytes for date type", byte_count)));
            }

            date_buf = (unsigned char *) palloc(VARHDRSZ + byte_count);
            memcpy(date_buf, VARDATA_ANY(inbytea), byte_count);
            date_byte_count = byte_count;

            switch(date_buf[0])
            {
                case 0x02:
                case 0x04:
                    date_byte_count--;
                    date_b = &date_buf[2];
                    memset(date_char_int.buf, '\0', 4);
                    while(date_byte_count--)
                        date_char_int.buf[date_byte_count] = *date_b++;

                    date_res = (Timestamp)(date_char_int.out - ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY)) * 1000000;

                    /* format as ISO 8601 JSON string */
                    {
                        char *date_str;
                        Datum timestamp_text;
                        StringInfoData json_buf;

                        timestamp_text = DirectFunctionCall1(timestamp_out, TimestampGetDatum(date_res));
                        date_str = DatumGetCString(timestamp_text);

                        initStringInfo(&json_buf);
                        appendStringInfoChar(&json_buf, '"');
                        appendStringInfoString(&json_buf, date_str);
                        appendStringInfoChar(&json_buf, '"');

                        PG_RETURN_TEXT_P(cstring_to_text(json_buf.data));
                    }
                    break;
                default:
                    PG_RETURN_NULL();
            }
            pfree(date_buf);
        }
        break;
    case 0x05: /* uuid (simple) */
        elog(LOG, "Unknown type 5");
        PG_RETURN_NULL();
        break;
    case 0x06: /* uuid (extended) */
        elog(LOG, "Unknown type 6");
            PG_RETURN_NULL();
        break;
    case 0x07:// reserved
        elog(LOG, "Unknown type 7");
        PG_RETURN_NULL();
        break;
    case 0x08:
        memcpy(buf, VARDATA_ANY(inbytea), UUID_LEN+1);
        uuid = (pg_uuid_t*)palloc(16);
        memcpy(uuid, buf+1, UUID_LEN);
        initStringInfo(&sbuf);
        for (i = 0; i < UUID_LEN; i++)
        {
            int                     hi;
            int                     lo;
            if (i == 4 || i == 6 || i == 8 || i == 10)
                    appendStringInfoChar(&sbuf, '-');

            hi = uuid->data[i] >> 4;
            lo = uuid->data[i] & 0x0F;

            appendStringInfoChar(&sbuf, hex_chars[hi]);
            appendStringInfoChar(&sbuf, hex_chars[lo]);
        }

        PG_RETURN_TEXT_P(cstring_to_text(sbuf.data));
        break;
    case 0x09:// reserved
        elog(LOG, "Unknown type 9");
        PG_RETURN_NULL();
        break;
    case 0x0A:// reserved
        elog(LOG, "Unknown type 10");
        PG_RETURN_NULL();
        break;
    case 0x0E:// ETH address saved as BYTEA
        memcpy((void *)res, /* destination */
                (void *) VARDATA_ANY(inbytea), /* source */
                VARSIZE_ANY_EXHDR(inbytea));       /* how many bytes */
        res[byte_count]='\0';
        res++;// shift the marker byte
        
        PG_RETURN_TEXT_P(cstring_to_text(res));
        break;
    case 0x0F://linkage data system LDS send params from concat |
        memcpy(buf, VARDATA_ANY(inbytea), byte_count+1);
        initStringInfo(&sbuf);
        for (i = 1; i < byte_count; i++)
        {
            int                     hi;
            int                     lo;

            hi = buf[i] >> 4;
            lo = buf[i] & 0x0F;

            appendStringInfoChar(&sbuf, hex_chars[hi]);
            appendStringInfoChar(&sbuf, hex_chars[lo]);
        }

        PG_RETURN_TEXT_P(cstring_to_text(sbuf.data));
        break;
    }
    PG_RETURN_NULL();
}

Datum
value_to_jsonb(PG_FUNCTION_ARGS)
{
    bytea       *inbytea;
    int         byte_count;
    char        *res;
    int         i;
    unsigned char *b;
    unsigned char *buf;
    unsigned char op_id;
    short params = 0;
    pg_uuid_t  *uuid;
    static const char hex_chars[] = "0123456789abcdef";
    StringInfoData sbuf;
    Jsonb       *result;
    char_int64 char_int;
    inbytea = PG_DETOAST_DATUM(PG_GETARG_BYTEA_P(0));
    byte_count = VARSIZE_ANY_EXHDR(inbytea);
    buf = (unsigned char *) palloc(VARHDRSZ + byte_count);

    /* Allocate memory for result and copy input data to buffer */
    res = (char *) palloc(byte_count + VARHDRSZ);
    SET_VARSIZE(res, byte_count + VARHDRSZ);
    memcpy(buf, VARDATA_ANY(inbytea), byte_count);

    /* Extract the 4-bit type identifier and 12-bit parameters from 2-byte header */
    op_id = (buf[0] & MASK) >> 4;
    params = ((buf[0] & 0x0F) << 8) | buf[1];

    /* Reject impossible op_id/params combinations before any decode logic */
    pg_ilib_check_header("value_to_jsonb", op_id, params, byte_count);

    switch(op_id){
    case 0x01: /* text */
        {
            int data_len  = byte_count - 2;
            /*
             * params semantics for text:
             *   0  → no declared length; use the entire payload (str_to_bytea writes 0)
             *   >0 → declared length hint; clamp to actual data available
             * Without this branch, params=0 would produce copy_bytes=0 (empty string).
             */
            int copy_bytes = (params == 0 || data_len <= (int)params)
                             ? data_len
                             : (int)params;

            /* null-terminate for escape_json, which handles JSON escaping of
             * special chars (", \, control chars) and adds surrounding quotes */
            char *text_copy = palloc(copy_bytes + 1);
            memcpy(text_copy, buf + 2, copy_bytes);
            text_copy[copy_bytes] = '\0';

            initStringInfo(&sbuf);
            escape_json(&sbuf, text_copy);
        }
        PG_RETURN_JSONB_P(DirectFunctionCall1(jsonb_in, CStringGetDatum(sbuf.data)));
        break;
    case 0x02: /* bigint */
        {            
            mpz_t big_number;
            char *binary_str;
            mpz_init(big_number);
            mpz_import(big_number, byte_count-2, 1, sizeof(buf[0]), 0, 0, &buf[2]);
            

            binary_str = mpz_get_str(NULL, 10, big_number);
            mpz_clear(big_number);

            if (params > 0) {
                size_t len = strlen(binary_str);
                char *decimal_str;

                if (params >= len) {
                    /* fewer digits than scale: e.g. 123 with scale=5 → "0.00123" */
                    int zeros_needed = params - len;
                    decimal_str = palloc(3 + zeros_needed + len);
                    strcpy(decimal_str, "0.");
                    for (int i = 0; i < zeros_needed; i++) {
                        decimal_str[2 + i] = '0';
                    }
                    strcpy(decimal_str + 2 + zeros_needed, binary_str);
                } else {
                    /* insert decimal point inside: e.g. 12345 scale=2 → "123.45" */
                    decimal_str = palloc(len + 2);
                    size_t int_part_len = len - params;
                    memcpy(decimal_str, binary_str, int_part_len);
                    decimal_str[int_part_len] = '.';
                    memcpy(decimal_str + int_part_len + 1, binary_str + int_part_len, params);
                    decimal_str[len + 1] = '\0';
                }

                res = decimal_str;
                free(binary_str); /* free GMP-allocated string */
            } else {
                /* params == 0: integer — return as-is */
                /* copy into palloc memory so we can free the GMP string */
                res = pstrdup(binary_str);
                free(binary_str);
            }

            PG_RETURN_JSONB_P(DirectFunctionCall1(jsonb_in, CStringGetDatum(res)));
        }
        break;
    case 0x03: /* bool */
        if(buf[2])            PG_RETURN_JSONB_P(DirectFunctionCall1(jsonb_in,CStringGetDatum("true")));
        else            PG_RETURN_JSONB_P(DirectFunctionCall1(jsonb_in,CStringGetDatum("false")));
        break;
    case 0x04: /* timestamp / date */
        {
            unsigned char *date_buf;
            Timestamp date_res;
            char_int32 date_char_int;
            char *date_b;
            int date_byte_count;

            if (byte_count > 6)
            {
                ereport(ERROR,
                    (errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
                    errmsg("Input of %d bytes has be max 5 bytes for date type", byte_count)));
            }

            date_buf = (unsigned char *) palloc(VARHDRSZ + byte_count);
            memcpy(date_buf, VARDATA_ANY(inbytea), byte_count);
            date_byte_count = byte_count;

            date_byte_count-=2;

            date_b = &buf[2];
            memset(date_char_int.buf, '\0', 4);

            while(date_byte_count--)
                date_char_int.buf[date_byte_count] = *date_b++;
            

            date_res = (Timestamp)(date_char_int.out - ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY)) * 1000000;

            /* format Unix timestamp as ISO 8601 JSON string */
            {
                char *date_str;
                Datum timestamp_text;
                StringInfoData json_buf;

                timestamp_text = DirectFunctionCall1(timestamp_out, TimestampGetDatum(date_res));
                date_str = DatumGetCString(timestamp_text);

                initStringInfo(&json_buf);
                appendStringInfoChar(&json_buf, '"');
                appendStringInfoString(&json_buf, date_str);
                appendStringInfoChar(&json_buf, '"');

                
                pfree(date_buf);
                PG_RETURN_JSONB_P(DirectFunctionCall1(jsonb_in, CStringGetDatum(json_buf.data)));
            }
        }
        break;
    case 0x05: /* uuid (simple) */
        elog(LOG, "Unknown type 5");
        PG_RETURN_NULL();
        break;
    case 0x06: /* uuid (extended) */
        elog(LOG, "Unknown type 6");
        PG_RETURN_NULL();
        break;
    case 0x07:// reserved
        elog(LOG, "Unknown type 7");
        PG_RETURN_NULL();
        break;
    case 0x08:
        uuid = (pg_uuid_t*)palloc(16);
        memcpy(uuid, buf+2, UUID_LEN);
        initStringInfo(&sbuf);
        appendStringInfoChar(&sbuf, '"');
        for (i = 0; i < UUID_LEN; i++)
        {
            int                     hi;
            int                     lo;
            if (i == 4 || i == 6 || i == 8 || i == 10)
                    appendStringInfoChar(&sbuf, '-');

            hi = uuid->data[i] >> 4;
            lo = uuid->data[i] & 0x0F;

            appendStringInfoChar(&sbuf, hex_chars[hi]);
            appendStringInfoChar(&sbuf, hex_chars[lo]);

        }
        appendStringInfoChar(&sbuf, '"');        PG_RETURN_JSONB_P(DirectFunctionCall1(jsonb_in,CStringGetDatum(sbuf.data)));
        break;
    case 0x09:// reserved
        elog(LOG, "Unknown type 9");
        PG_RETURN_NULL();
        break;
    case 0x0A:// reserved
        elog(LOG, "Unknown type 10");
        PG_RETURN_NULL();
        break;
    case 0x0E: // JSON

        op_id = (buf[0] & MASK) >> 4;
        params = ((buf[0] & 0x0F) << 8) | buf[1];


        if (op_id != 0x0e) {
            elog(ERROR, "Invalid op_id: expected 0x0e, got 0x%02x", op_id);
            PG_RETURN_NULL();
        }

        /* JSONB payload size (after 2-byte header) */

        /* reconstruct Jsonb from raw binary JSONB bytes */
        result = (Jsonb *)palloc(VARHDRSZ + byte_count - 2);
        SET_VARSIZE(result, VARHDRSZ + byte_count - 2);
        
        /* copy raw JSONB bytes */
        memcpy(VARDATA(result), buf + 2, byte_count - 2);        
        
        PG_RETURN_JSONB_P(result);
        
        break;
    case 0x0F: /* hex bytes */
        initStringInfo(&sbuf);
        appendStringInfoChar(&sbuf, 34); /* opening '"' */
        appendStringInfoString(&sbuf, "0x");
        for (i = 2; i < byte_count; i++)
        {
            int hi = buf[i] >> 4;
            int lo = buf[i] & 0x0F;
            appendStringInfoChar(&sbuf, hex_chars[hi]);
            appendStringInfoChar(&sbuf, hex_chars[lo]);
        }
        appendStringInfoChar(&sbuf, 34); /* closing '"' */

        PG_RETURN_JSONB_P(DirectFunctionCall1(jsonb_in, CStringGetDatum(sbuf.data)));
    }
}

Datum
bytea_to_bool(PG_FUNCTION_ARGS)
{
    bytea       *inbytea;
    int         byte_count;
    unsigned char *buf;
    unsigned char op_id;
    short params = 0;

    inbytea = PG_DETOAST_DATUM(PG_GETARG_BYTEA_P(0));
    byte_count = VARSIZE_ANY_EXHDR(inbytea);
    buf = (unsigned char *) palloc(VARHDRSZ + byte_count);
    memcpy(buf, VARDATA_ANY(inbytea),byte_count);
    op_id = (buf[0] & MASK) >> 4;
    params = ((buf[0] & 0x0F) << 8) | buf[1];

    if (op_id == 3){
        pg_ilib_check_header("bytea_to_bool", op_id, params, byte_count);
        PG_RETURN_BOOL(buf[2] != 0x00);
    }else
       PG_RETURN_NULL();
}

Datum
bool_to_bytea(PG_FUNCTION_ARGS)
{
    bool        val = PG_GETARG_BOOL(0);
    bytea       *outbytea;
    char        *ptr;
    char        buf[3];
    outbytea = (bytea*)palloc(VARHDRSZ + 3);
    SET_VARSIZE(outbytea, VARHDRSZ + 3);
    buf[0] = 0x30;
    buf[1] = 0x00;
    if (val)  /* true */
        buf[2] = 0xff;
    else      /* false */
        buf[2] = 0x00;
    
    ptr = buf;
    memcpy(VARDATA(outbytea), (void*)ptr, 3);
    PG_RETURN_BYTEA_P(outbytea);
}

/*
 * decode_timestamp_from_bytea() — shared helper
 *
 * Reads op_id=0x04 bytea and returns:
 *   pg_ts_out       — PostgreSQL Timestamp (microseconds since 2000), no tz
 *   tz_minutes_out  — timezone offset in minutes (from params field)
 *
 * Returns true on success, false if op_id mismatch or bad input.
 */
static bool
decode_timestamp_from_bytea(bytea *inbytea,
                             Timestamp *pg_ts_out,
                             int *tz_minutes_out)
{
    char_int32    char_int;
    unsigned char buf[8];
    unsigned char *b;
    int           byte_count;
    unsigned char op_id;
    short         params;
    uint32_t      unix_timestamp_sec;

    byte_count = VARSIZE_ANY_EXHDR(inbytea);
    if (byte_count < 6)
        return false;

    memset(char_int.buf, '\0', sizeof(char_int.buf));
    memcpy(buf, VARDATA_ANY(inbytea), 6);

    op_id  = (buf[0] & MASK) >> 4;
    params = ((buf[0] & 0x0F) << 8) | buf[1];

    if (op_id != 4)
        return false;

    /* Validate params (tz offset) is in range before using it */
    pg_ilib_check_header("bytea_to_timestamp[tz]", op_id, params,
                         VARSIZE_ANY_EXHDR(inbytea));

    /* Read 4 big-endian payload bytes into little-endian char_int */
    byte_count = 4;
    b = &buf[2];
    while (byte_count--)
        char_int.buf[byte_count] = *b++;

    unix_timestamp_sec = char_int.out;

    /* Unix (seconds since 1970) → PostgreSQL (microseconds since 2000) */
    *pg_ts_out = ((int64)unix_timestamp_sec -
                  ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY))
                 * USECS_PER_SEC;

    *tz_minutes_out = (int)(short)params;
    return true;
}

/*
 * bytea_to_timestamp(bytea) → timestamp
 *
 * Returns a plain timestamp (no time zone).  Timezone offset stored in params
 * is intentionally IGNORED — the raw UTC-equivalent value is returned as-is.
 * This is the correct return type for CREATE CAST (bytea AS timestamp).
 */
Datum
bytea_to_timestamp(PG_FUNCTION_ARGS)
{
    bytea     *inbytea = PG_DETOAST_DATUM(PG_GETARG_BYTEA_P(0));
    Timestamp  pg_ts;
    int        tz_minutes;

    if (!decode_timestamp_from_bytea(inbytea, &pg_ts, &tz_minutes))
        PG_RETURN_NULL();

    PG_RETURN_TIMESTAMP(pg_ts);
}

/*
 * bytea_to_timestamptz(bytea) → timestamp with time zone
 *
 * Returns a timezone-aware timestamp.  The tz offset stored in params (signed
 * minutes from UTC) is applied so the absolute point in time is preserved.
 * Use this for explicit timezone-aware retrieval or for
 * CREATE CAST (bytea AS timestamptz).
 */
Datum
bytea_to_timestamptz(PG_FUNCTION_ARGS)
{
    bytea       *inbytea = PG_DETOAST_DATUM(PG_GETARG_BYTEA_P(0));
    Timestamp    pg_ts;
    TimestampTz  result_ts;
    int          tz_minutes;

    if (!decode_timestamp_from_bytea(inbytea, &pg_ts, &tz_minutes))
        PG_RETURN_NULL();

    /* Subtract the stored offset so result is correct UTC-based timestamptz */
    result_ts = pg_ts - ((int64)tz_minutes * 60 * USECS_PER_SEC);

    PG_RETURN_TIMESTAMPTZ(result_ts);
}

Datum
timestamp_to_bytea(PG_FUNCTION_ARGS)
{
    bytea       *outbytea;
    unsigned char *data;
    uint32_t    timestamp_sec;
    short       params;
    
    /* value is a Unix timestamp in seconds */
    int64 value = PG_GETARG_INT64(0);
    int timezone_offset = PG_GETARG_INT32(1); /* signed minutes from UTC */

    
    timestamp_sec = (uint32_t)value;
    
    params = (short)timezone_offset;
    
    /* allocate: 2-byte header + 4-byte Unix timestamp = 6 bytes */
    outbytea = (bytea*)palloc(VARHDRSZ + 6);
    SET_VARSIZE(outbytea, VARHDRSZ + 6);
    
    data = (unsigned char *)VARDATA(outbytea);
    
    /* header: op_id=4 in high nibble, tz offset in params */
    data[0] = (0x04 << 4) | ((params >> 8) & 0x0F);
    data[1] = params & 0xFF;
    
    /* Unix timestamp in big-endian order */
    data[2] = (timestamp_sec >> 24) & 0xFF;
    data[3] = (timestamp_sec >> 16) & 0xFF;
    data[4] = (timestamp_sec >> 8) & 0xFF;
    data[5] = timestamp_sec & 0xFF;
    
    
    PG_RETURN_BYTEA_P(outbytea);
}

Datum
bytea_to_jsonb(PG_FUNCTION_ARGS)
{
    bytea       *inbytea;
    Jsonb       *result;
    unsigned char *buf;
    int         byte_count;
    unsigned char op_id;
    short       params = 0;
    int         jsonb_size;

    inbytea = PG_DETOAST_DATUM(PG_GETARG_BYTEA_P(0));
    byte_count = VARSIZE_ANY_EXHDR(inbytea);
    
    if (byte_count < 2) {
        elog(ERROR, "Invalid bytea: too short");
        PG_RETURN_NULL();
    }

    buf = (unsigned char *)VARDATA_ANY(inbytea);
    
    /* parse 2-byte header */
    op_id = (buf[0] & MASK) >> 4;
    params = ((buf[0] & 0x0F) << 8) | buf[1];
    

    if (op_id != 0x0e) {
        elog(ERROR, "Invalid op_id: expected 0x0e, got 0x%02x", op_id);
        PG_RETURN_NULL();
    }

    pg_ilib_check_header("bytea_to_jsonb", op_id, params, byte_count);

    /* JSONB payload size (after the 2-byte header) */
    jsonb_size = byte_count - 2;
    
    /* reconstruct Jsonb from raw binary JSONB bytes */
    result = (Jsonb *)palloc(VARHDRSZ + jsonb_size);
    SET_VARSIZE(result, VARHDRSZ + jsonb_size);
    
    /* copy raw JSONB bytes */
    memcpy(VARDATA(result), buf + 2, jsonb_size);
    
    
    PG_RETURN_JSONB_P(result);
}

Datum
jsonb_to_bytea(PG_FUNCTION_ARGS)
{
    Jsonb       *jb = PG_GETARG_JSONB_P(0);
    bytea       *result;
    int         jb_size = VARSIZE(jb) - VARHDRSZ;
    short       params = 0;
    
    /* allocate: 2-byte header + raw JSONB bytes */
    result = (bytea*)palloc(VARHDRSZ + 2 + jb_size);
    SET_VARSIZE(result, VARHDRSZ + 2 + jb_size);
    
    unsigned char *data = (unsigned char*)VARDATA(result);
    
    /* header: op_id=0x0E (jsonb) */
    data[0] = (0x0e << 4) | ((params >> 8) & 0x0F);
    data[1] = params & 0xFF;
    
    /* copy raw binary JSONB directly */
    memcpy(data + 2, VARDATA(jb), jb_size);
    
    PG_RETURN_BYTEA_P(result);
}

Datum
str_to_bytea(PG_FUNCTION_ARGS)
{
    bytea         *outbytea;
    int            text_len;
    int            byte_count;
    unsigned char *data;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    text *value = PG_GETARG_TEXT_PP(0);
    text_len  = VARSIZE_ANY_EXHDR(value);
    byte_count = 2 + text_len; /* 2-byte header + UTF-8 text */

    outbytea = (bytea *) palloc(VARHDRSZ + byte_count);
    SET_VARSIZE(outbytea, VARHDRSZ + byte_count);

    data = (unsigned char *) VARDATA(outbytea);

    /* 2-byte header: op_id=0x01 (text), params=0
     * Byte 0: (op_id << 4) | (params >> 8)  = 0x10
     * Byte 1:  params & 0xFF                 = 0x00
     * Same layout as all other _to_bytea functions. */
    data[0] = (0x01 << 4); /* 0x10 */
    data[1] = 0x00;

    memcpy(data + 2, VARDATA_ANY(value), text_len);

    PG_RETURN_BYTEA_P(outbytea);
}

Datum
bytea_to_uuid(PG_FUNCTION_ARGS)
{
    bytea       *inbytea;
    pg_uuid_t   *uuid;
    char field_type[UUID_LEN+2];
    unsigned char  op_id;
    unsigned char  *buf;
    short params = 0;
    int byte_count;
    inbytea = PG_DETOAST_DATUM(PG_GETARG_BYTEA_P(0));
    byte_count = VARSIZE_ANY_EXHDR(inbytea);

    buf = (unsigned char*) palloc(VARHDRSZ + byte_count+1);
    memcpy(buf, VARDATA_ANY(inbytea), byte_count+1);

    op_id = (buf[0] & MASK)>>4;
    params = ((buf[0] & 0x0F) << 8) | buf[1];

    if (VARSIZE_ANY_EXHDR(inbytea) != UUID_LEN+2)
    {
        PG_RETURN_NULL();
    }    if(op_id == 0x08){
        pg_ilib_check_header("bytea_to_uuid", op_id, params, byte_count);
        uuid = (pg_uuid_t*)palloc(VARHDRSZ + 16);
        memcpy(uuid, buf+2, UUID_LEN);
        PG_RETURN_UUID_P(uuid);
    }else{
        PG_RETURN_NULL();
    }
}

Datum
uuid_to_bytea(PG_FUNCTION_ARGS)
{
    bytea       *outbytea;
    pg_uuid_t   *uuid;
    char field_type[UUID_LEN+2];
    
    uuid = PG_GETARG_UUID_P(0);
    short params = (short)PG_GETARG_INT32(1);

    field_type[0] = (0x08 << 4) | ((params >> 8) & 0x0F);
    field_type[1] = params & 0xFF;

    /* uuid isn't TOASTABLE */

    memcpy(field_type+2,uuid,UUID_LEN);

    outbytea = (bytea*)palloc(VARHDRSZ + UUID_LEN+2);
    SET_VARSIZE(outbytea, VARHDRSZ + UUID_LEN+2);
    memcpy(VARDATA(outbytea), (void*)field_type, UUID_LEN+2);
    PG_RETURN_BYTEA_P(outbytea);
}

unsigned char HexChar (char c);
int HexToBin (const char* s, unsigned char * buff, int length);

unsigned char HexChar (char c)
{
    if ('0' <= c && c <= '9') return (unsigned char)(c - '0');
    if ('A' <= c && c <= 'F') return (unsigned char)(c - 'A' + 10);
    if ('a' <= c && c <= 'f') return (unsigned char)(c - 'a' + 10);
    return 0xFF;
}

int HexToBin (const char* s, unsigned char *buff, int length)
{
    int result;
    unsigned char msn;
    unsigned char lsn;
    unsigned char bin;
    if (!s || !buff || length <= 0) return -1;

    for (result = 0; *s; ++result)
    {
        msn = HexChar(*s++);
        if (msn == 0xFF) return -1;
            lsn = HexChar(*s++);
        if (lsn == 0xFF) return -1;
            bin = (msn << 4) + lsn;

        if (length-- <= 0) return -1;
        *buff++ = bin;
    }
    return result;
}

Datum
hex_to_bytea(PG_FUNCTION_ARGS)
{
    bytea       *outbytea;
    char        *field_type;
    int         hex_len, data_bytes;
    short       params = 0;
    const char  *intext;
    text        *value = PG_GETARG_TEXT_PP(0);
    params = (short)PG_GETARG_INT32(1);

    intext = text_to_cstring(value);
    hex_len = strlen(intext);
    

    /* validate "0x" prefix */
    if (hex_len < 2 || intext[0] != '0' || intext[1] != 'x') {
        elog(ERROR, "Wrong hex format - must start with 0x");
        PG_RETURN_NULL();
    }
    
    /* hex string must have an even number of nibbles */
    if ((hex_len - 2) % 2) {
        elog(ERROR, "Odd number of hex characters");
        PG_RETURN_NULL();
    }
    
    /* payload byte count (hex digits / 2, excluding "0x") */
    data_bytes = (hex_len - 2) / 2;
    
    /* allocate: 2-byte header + binary payload */
    field_type = (char*)palloc(2 + data_bytes);
    
    /* write 2-byte header */
    field_type[0] = (0x0f << 4) | ((params >> 8) & 0x0F);
    field_type[1] = params & 0xFF;
    
    if (HexToBin(intext + 2, (unsigned char*)&field_type[2], data_bytes) < 0) {
        elog(ERROR, "Failed to convert hex string");
        pfree(field_type);
        PG_RETURN_NULL();
    }
    
    {
        char hex_dump[256];
        int offset = 0;
        for (int i = 0; i < 2 + data_bytes && offset < 250; i++) {
            offset += sprintf(hex_dump + offset, "%02x ", 
                            (unsigned char)field_type[i]);
        }
    }
    
    /* build result bytea */
    outbytea = (bytea*)palloc(VARHDRSZ + 2 + data_bytes);
    SET_VARSIZE(outbytea, VARHDRSZ + 2 + data_bytes);
    memcpy(VARDATA(outbytea), field_type, 2 + data_bytes);
    
    pfree(field_type);
    PG_RETURN_BYTEA_P(outbytea);
}
/*
Datum
bytea_to_hex(PG_FUNCTION_ARGS)
{
    bytea         *inbytea;
    unsigned char *buf;
    StringInfoData sbuf;
    int            i=0;
    int            byte_count=0;
    short          params=0;
    unsigned char  op_id;
    static const char hex_chars[] = "0123456789abcdef";
    inbytea = PG_DETOAST_DATUM(PG_GETARG_BYTEA_P(0));
    byte_count = VARSIZE_ANY_EXHDR(inbytea);    buf = (unsigned char *) palloc(VARHDRSZ + byte_count);
    memcpy(buf, VARDATA_ANY(inbytea), byte_count+2);
    op_id = (buf[0] & MASK)>>4;
    params = ((buf[0] & 0x0F) << 8) | buf[1];
    if(op_id==0x0f){
        initStringInfo(&sbuf);
        appendStringInfoChar(&sbuf, '0');
        appendStringInfoChar(&sbuf, 'x');
        for (i = 2; i < byte_count; i++)
        {
            int                     hi;
            int                     lo;

            hi = buf[i] >> 4;
            lo = buf[i] & 0x0F;

            appendStringInfoChar(&sbuf, hex_chars[hi]);
            appendStringInfoChar(&sbuf, hex_chars[lo]);
        }
        pfree(buf);
        PG_RETURN_TEXT_P(cstring_to_text(sbuf.data));
    }
}
*/
Datum
bytea_to_hex(PG_FUNCTION_ARGS)
{
    bytea           *inbytea;
    unsigned char   *buf;
    StringInfoData  sbuf;
    int             i;
    int             byte_count;
    short           params = 0;
    unsigned char   op_id;
    static const char hex_chars[] = "0123456789abcdef";
    
    /* NULL check */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();
    
    inbytea = PG_DETOAST_DATUM(PG_GETARG_BYTEA_P(0));
    byte_count = VARSIZE_ANY_EXHDR(inbytea);
    
    /* minimum size check */
    if (byte_count < 2) {
        elog(ERROR, "bytea_to_hex: input too small (%d bytes, need at least 2)", byte_count);
    }
    
    /* allocate output buffer */
    buf = (unsigned char *)palloc(byte_count);
    
    /* copy exactly byte_count bytes from detoasted input */
    memcpy(buf, VARDATA_ANY(inbytea), byte_count);
    
    /* parse 2-byte header */
    op_id = (buf[0] & MASK) >> 4;
    params = ((buf[0] & 0x0F) << 8) | buf[1];
    
    elog(LOG, "bytea_to_hex: op_id=0x%02x, params=0x%03x, byte_count=%d", 
         op_id, params, byte_count);
    
    /* only op_id=0x0F (hex) is valid here */
    if (op_id == 0x0f) {
        initStringInfo(&sbuf);
        appendStringInfoChar(&sbuf, '0');
        appendStringInfoChar(&sbuf, 'x');
        
        for (i = 2; i < byte_count; i++) {
            int hi = buf[i] >> 4;
            int lo = buf[i] & 0x0F;
            
            appendStringInfoChar(&sbuf, hex_chars[hi]);
            appendStringInfoChar(&sbuf, hex_chars[lo]);
        }
        
        pfree(buf);
        PG_RETURN_TEXT_P(cstring_to_text(sbuf.data));
    } else {
        /* unexpected op_id — fall through to error */
        pfree(buf);
        elog(ERROR, "bytea_to_hex: unsupported op_id=0x%02x (expected 0x0f)", op_id);
        PG_RETURN_NULL();  /* unreachable: ereport above never returns */
    }
}

Datum
date_to_bytea(PG_FUNCTION_ARGS)
{
    Oid        typid;
    Oid        collid;
    int32      typmod = -1;
    int        tz = 0;
    short      params;
    text       *datetime = PG_GETARG_TEXT_PP(0);
    text       *tmplate = PG_GETARG_TEXT_PP(1);
    int timezone_offset = PG_GETARG_INT32(2); /* signed minutes from UTC */

    bool       have_error = false;
    Datum      value;
    uint32     tm = 0;
    
    bytea       *outbytea;
    char        *ptr;
    char        buf[bufsize+1];                /* bigger than needed, but reasonable */
    int         byte_count;
    /*
    static const char *fmt_str[] =
    {
        "yyyy-mm-dd",
        "dd.mm.yyyy",
        "dd.mm.yyyy HH24:MI:SS",
        "HH24:MI:SS TZH:TZM",
        "HH24:MI:SS TZH",
        "HH24:MI:SS",
        "yyyy-mm-dd HH24:MI:SS TZH:TZM",
        "yyyy-mm-dd HH24:MI:SS TZH",
        "yyyy-mm-dd HH24:MI:SS"
        "YYYY-MM-DD HH24:MI:SS.USTZH" --for now()::text
    };
    */
    
    params = (short)timezone_offset;
    collid = DEFAULT_COLLATION_OID;    value = parse_datetime(datetime, tmplate, collid, true, &typid, &typmod, &tz, &have_error);

    /* have_error was computed but never checked - a genuine parse failure
     * and a successfully-parsed 1970-01-01 00:00:00 UTC (tm == 0, since tm
     * is seconds-from-Unix-epoch) were indistinguishable below, so the
     * epoch date silently encoded as NULL instead of a valid zero-value
     * bytea. Check have_error explicitly instead of inferring failure from
     * tm being falsy. */
    if (have_error)
        PG_RETURN_NULL();

    switch(typid){
        case 1082:
            tm = DatumGetTimestamp(value)
                * 86400
                +((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);

            break;/* Date */
        case 1083:
        case 1114:
        case 1184:
        case 1266:
            tm = DatumGetTimestamp(value)
                /1000000 +
                ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
            break;/* time, timestamp, same + tz */
        default:
            /* parse_datetime succeeded but returned a type we don't encode */
            PG_RETURN_NULL();
    }

    {
        byte_count = 6;
        outbytea = (bytea*)palloc(VARHDRSZ + byte_count);
        SET_VARSIZE(outbytea, VARHDRSZ + byte_count);
        /* write integer digits into buffer right-to-left */
        ptr = buf+byte_count;
        do
        {
            *--ptr = tm % HEXBASE;
            tm /= HEXBASE;
        } while (ptr > buf+1 && tm);

        *--ptr = params & 0xFF;
        *--ptr = (0x04 << 4) | ((params >> 8) & 0x0F);

        memcpy(VARDATA(outbytea), (void*)ptr, byte_count);

        PG_RETURN_BYTEA_P(outbytea);
    }
}
/* POSTGRES_EPOCH_JDATE = 2451545  (2000-01-01)
 * UNIX_EPOCH_JDATE     = 2440588  (1970-01-01) */
Datum
bytea_to_date(PG_FUNCTION_ARGS)
{
    bytea         *inbytea;
    int           byte_count;                                   
    Timestamp     res;
    unsigned char bbuf[6];
    unsigned char *b;
    unsigned char op_id;
    short params = 0;
    char_int32 char_int;                                      
                                                              
    inbytea = PG_DETOAST_DATUM(PG_GETARG_BYTEA_P(0));         
    byte_count = VARSIZE_ANY_EXHDR(inbytea);    

    if (byte_count > 6)
    {
        PG_RETURN_NULL();
        ereport(ERROR,
            (errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
             errmsg("Input of %d bytes has be max 4 bytes (32 bits) in size", (int)VARSIZE_ANY_EXHDR(inbytea) )));
    }
    
    memcpy(bbuf, VARDATA_ANY(inbytea),byte_count);
    byte_count-=2;

    op_id = (bbuf[0]&MASK)>>4;
    params = ((bbuf[0] & 0x0F) << 8) | bbuf[1];

    switch(op_id){
        case 0x02:
        case 0x04:
            b = &bbuf[2];
            memset(char_int.buf,'\0',4);
            while(byte_count--)
                char_int.buf[byte_count] = *b++;
            res = (Timestamp)(char_int.out-((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY))*1000000;
            PG_RETURN_TIMESTAMP(res);
            break;
        default:
            PG_RETURN_NULL();
    }
}

/*
 * cmp_date — compare two bytea-encoded dates
 * Returns: -1 (left < right), 0 (equal), 1 (left > right)
 */
Datum
cmp_date(PG_FUNCTION_ARGS)
{
    bytea *left_arg;
    bytea *right_arg;
    int32 left_val,right_val;
    char_int32 char_int;
    char *b;
    
    char lbuf[6];
    char rbuf[6];
    int byte_count;
    left_arg = PG_DETOAST_DATUM(PG_GETARG_BYTEA_P(0));
    right_arg = PG_DETOAST_DATUM(PG_GETARG_BYTEA_P(1));
    byte_count = VARSIZE_ANY_EXHDR(left_arg);
    if (byte_count > 6){
        PG_RETURN_NULL();
        ereport(ERROR,
            (errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
             errmsg("Input of %d bytes has be max 4 bytes (32 bits) in size", (int)VARSIZE_ANY_EXHDR(left_arg) )));
    }
    byte_count = VARSIZE_ANY_EXHDR(right_arg);
    if (byte_count > 6){
        PG_RETURN_NULL();
        ereport(ERROR,
            (errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
             errmsg("Input of %d bytes has be max 4 bytes (32 bits) in size", (int)VARSIZE_ANY_EXHDR(right_arg) )));
    }
    
    memcpy(lbuf, VARDATA_ANY(left_arg),byte_count);
    memcpy(rbuf, VARDATA_ANY(right_arg),byte_count);
    
    
    if((lbuf[0]|0x60)!=0x60){
        PG_RETURN_NULL();
        ereport(ERROR,
            (errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
             errmsg("Unrecognized input format left value")));
    }
    if((rbuf[0]|0x60)!=0x60){
        PG_RETURN_NULL();
        ereport(ERROR,
            (errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
             errmsg("Unrecognized input format right value")));
    }
    byte_count=4;
    b = &lbuf[2];
    memset(char_int.buf,'\0',4);
    while(byte_count--)
        char_int.buf[byte_count] = *b++;
    left_val = char_int.out;
    byte_count=4;
    b = &rbuf[2];
    memset(char_int.buf,'\0',4);
    while(byte_count--)
        char_int.buf[byte_count] = *b++;
    right_val = char_int.out;

    if(left_val>right_val)
        PG_RETURN_INT32(1);
    if(left_val<right_val)
        PG_RETURN_INT32(-1);
    else
        PG_RETURN_INT32(0);
}
