#include "postgres.h"

#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "bitutils.h"
#include "bitvec.h"
#include "catalog/pg_type.h"
#include "common/shortest_dec.h"
#include "fmgr.h"
#include "halfutils.h"
#include "halfvec.h"
#include "hnsw.h"
#include "ivfflat.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "libpq/libpq.h"
#include "executor/spi.h"
#include "port.h" /* for strtof() */
#include "sparsevec.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/float.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "vector.h"
#include "storage/fd.h"
#include "commands/tablecmds.h"

#include "diskann.h"
#include "vamana.h"
#include "vamana_index.h"
#include "new_vector.h"
#include "my_vector.h"
#include "store_index.h"
#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#include "bit_array.h"
#endif

#define STATE_DIMS(x) (ARR_DIMS(x)[0] - 1)
#define CreateStateDatums(dim) palloc(sizeof(Datum) * (dim + 1))

#if defined(USE_TARGET_CLONES) && !defined(__FMA__)
#define VECTOR_TARGET_CLONES __attribute__((target_clones("default", "fma")))
#else
#define VECTOR_TARGET_CLONES
#endif

PG_MODULE_MAGIC;

/*
 * Initialize index options and variables
 */
PGDLLEXPORT void _PG_init(void);
void _PG_init(void)
{
	BitvecInit();
	HalfvecInit();
	HnswInit();
	IvfflatInit();
}

/*
 * Ensure same dimensions
 */
static inline void
CheckDims(Vector *a, Vector *b)
{
	if (a->dim != b->dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("different vector dimensions %d and %d", a->dim, b->dim)));
}

/*
 * Ensure expected dimensions
 */
static inline void
CheckExpectedDim(int32 typmod, int dim)
{
	if (typmod != -1 && typmod != dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected %d dimensions, not %d", typmod, dim)));
}

/*
 * Ensure valid dimensions
 */
static inline void
CheckDim(int dim)
{
	if (dim < 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("vector must have at least 1 dimension")));

	if (dim > VECTOR_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("vector cannot have more than %d dimensions", VECTOR_MAX_DIM)));
}

/*
 * Ensure finite element
 */
static inline void
CheckElement(float value)
{
	if (isnan(value))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("NaN not allowed in vector")));

	if (isinf(value))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("infinite value not allowed in vector")));
}

/*
 * Allocate and initialize a new vector
 */
Vector *
InitVector(int dim)
{
	Vector *result;
	int size;

	size = VECTOR_SIZE(dim);
	result = (Vector *)palloc0(size);
	SET_VARSIZE(result, size);
	result->dim = dim;

	return result;
}

/*
 * Check for whitespace, since array_isspace() is static
 */
static inline bool
vector_isspace(char ch)
{
	if (ch == ' ' ||
		ch == '\t' ||
		ch == '\n' ||
		ch == '\r' ||
		ch == '\v' ||
		ch == '\f')
		return true;
	return false;
}

/*
 * Check state array
 */
static float8 *
CheckStateArray(ArrayType *statearray, const char *caller)
{
	if (ARR_NDIM(statearray) != 1 ||
		ARR_DIMS(statearray)[0] < 1 ||
		ARR_HASNULL(statearray) ||
		ARR_ELEMTYPE(statearray) != FLOAT8OID)
		elog(ERROR, "%s: expected state array", caller);
	return (float8 *)ARR_DATA_PTR(statearray);
}

/*
 * Convert textual representation to internal representation
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_in);
Datum vector_in(PG_FUNCTION_ARGS)
{
	char *lit = PG_GETARG_CSTRING(0);
	int32 typmod = PG_GETARG_INT32(2);
	float x[VECTOR_MAX_DIM];
	int dim = 0;
	char *pt = lit;
	Vector *result;

	while (vector_isspace(*pt))
		pt++;

	if (*pt != '[')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type vector: \"%s\"", lit),
				 errdetail("Vector contents must start with \"[\".")));

	pt++;

	while (vector_isspace(*pt))
		pt++;

	if (*pt == ']')
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("vector must have at least 1 dimension")));

	for (;;)
	{
		float val;
		char *stringEnd;

		if (dim == VECTOR_MAX_DIM)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("vector cannot have more than %d dimensions", VECTOR_MAX_DIM)));

		while (vector_isspace(*pt))
			pt++;

		/* Check for empty string like float4in */
		if (*pt == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type vector: \"%s\"", lit)));

		errno = 0;

		/* Use strtof like float4in to avoid a double-rounding problem */
		/* Postgres sets LC_NUMERIC to C on startup */
		val = strtof(pt, &stringEnd);

		if (stringEnd == pt)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type vector: \"%s\"", lit)));

		/* Check for range error like float4in */
		if (errno == ERANGE && isinf(val))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("\"%s\" is out of range for type vector", pnstrdup(pt, stringEnd - pt))));

		CheckElement(val);
		x[dim++] = val;

		pt = stringEnd;

		while (vector_isspace(*pt))
			pt++;

		if (*pt == ',')
			pt++;
		else if (*pt == ']')
		{
			pt++;
			break;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type vector: \"%s\"", lit)));
	}

	/* Only whitespace is allowed after the closing brace */
	while (vector_isspace(*pt))
		pt++;

	if (*pt != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type vector: \"%s\"", lit),
				 errdetail("Junk after closing right brace.")));

	CheckDim(dim);
	CheckExpectedDim(typmod, dim);

	result = InitVector(dim);
	for (int i = 0; i < dim; i++)
		result->x[i] = x[i];

	PG_RETURN_POINTER(result);
}

#define AppendChar(ptr, c) (*(ptr)++ = (c))
#define AppendFloat(ptr, f) ((ptr) += float_to_shortest_decimal_bufn((f), (ptr)))

/*
 * Convert internal representation to textual representation
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_out);
Datum vector_out(PG_FUNCTION_ARGS)
{
	Vector *vector = PG_GETARG_VECTOR_P(0);
	int dim = vector->dim;
	char *buf;
	char *ptr;

	/*
	 * Need:
	 *
	 * dim * (FLOAT_SHORTEST_DECIMAL_LEN - 1) bytes for
	 * float_to_shortest_decimal_bufn
	 *
	 * dim - 1 bytes for separator
	 *
	 * 3 bytes for [, ], and \0
	 */
	buf = (char *)palloc(FLOAT_SHORTEST_DECIMAL_LEN * dim + 2);
	ptr = buf;

	AppendChar(ptr, '[');

	for (int i = 0; i < dim; i++)
	{
		if (i > 0)
			AppendChar(ptr, ',');

		AppendFloat(ptr, vector->x[i]);
	}

	AppendChar(ptr, ']');
	*ptr = '\0';

	PG_FREE_IF_COPY(vector, 0);
	PG_RETURN_CSTRING(buf);
}

/*
 * Print vector - useful for debugging
 */
void PrintVector(char *msg, Vector *vector)
{
	char *out = DatumGetPointer(DirectFunctionCall1(vector_out, PointerGetDatum(vector)));

	elog(INFO, "%s = %s", msg, out);
	pfree(out);
}

/*
 * Convert type modifier
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_typmod_in);
Datum vector_typmod_in(PG_FUNCTION_ARGS)
{
	ArrayType *ta = PG_GETARG_ARRAYTYPE_P(0);
	int32 *tl;
	int n;

	tl = ArrayGetIntegerTypmods(ta, &n);

	if (n != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid type modifier")));

	if (*tl < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dimensions for type vector must be at least 1")));

	if (*tl > VECTOR_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dimensions for type vector cannot exceed %d", VECTOR_MAX_DIM)));

	PG_RETURN_INT32(*tl);
}

/*
 * Convert external binary representation to internal representation
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_recv);
Datum vector_recv(PG_FUNCTION_ARGS)
{
	StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
	int32 typmod = PG_GETARG_INT32(2);
	Vector *result;
	int16 dim;
	int16 unused;

	dim = pq_getmsgint(buf, sizeof(int16));
	unused = pq_getmsgint(buf, sizeof(int16));

	CheckDim(dim);
	CheckExpectedDim(typmod, dim);

	if (unused != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected unused to be 0, not %d", unused)));

	result = InitVector(dim);
	for (int i = 0; i < dim; i++)
	{
		result->x[i] = pq_getmsgfloat4(buf);
		CheckElement(result->x[i]);
	}

	PG_RETURN_POINTER(result);
}

/*
 * Convert internal representation to the external binary representation
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_send);
Datum vector_send(PG_FUNCTION_ARGS)
{
	Vector *vec = PG_GETARG_VECTOR_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint(&buf, vec->dim, sizeof(int16));
	pq_sendint(&buf, vec->unused, sizeof(int16));
	for (int i = 0; i < vec->dim; i++)
		pq_sendfloat4(&buf, vec->x[i]);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Convert vector to vector
 * This is needed to check the type modifier
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector);
Datum vector(PG_FUNCTION_ARGS)
{
	Vector *vec = PG_GETARG_VECTOR_P(0);
	int32 typmod = PG_GETARG_INT32(1);

	CheckExpectedDim(typmod, vec->dim);

	PG_RETURN_POINTER(vec);
}

/*
 * Convert array to vector
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(array_to_vector);
Datum array_to_vector(PG_FUNCTION_ARGS)
{
	ArrayType *array = PG_GETARG_ARRAYTYPE_P(0);
	int32 typmod = PG_GETARG_INT32(1);
	Vector *result;
	int16 typlen;
	bool typbyval;
	char typalign;
	Datum *elemsp;
	int nelemsp;

	if (ARR_NDIM(array) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("array must be 1-D")));

	if (ARR_HASNULL(array) && array_contains_nulls(array))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("array must not contain nulls")));

	get_typlenbyvalalign(ARR_ELEMTYPE(array), &typlen, &typbyval, &typalign);
	deconstruct_array(array, ARR_ELEMTYPE(array), typlen, typbyval, typalign, &elemsp, NULL, &nelemsp);

	CheckDim(nelemsp);
	CheckExpectedDim(typmod, nelemsp);

	result = InitVector(nelemsp);

	if (ARR_ELEMTYPE(array) == INT4OID)
	{
		for (int i = 0; i < nelemsp; i++)
			result->x[i] = DatumGetInt32(elemsp[i]);
	}
	else if (ARR_ELEMTYPE(array) == FLOAT8OID)
	{
		for (int i = 0; i < nelemsp; i++)
			result->x[i] = DatumGetFloat8(elemsp[i]);
	}
	else if (ARR_ELEMTYPE(array) == FLOAT4OID)
	{
		for (int i = 0; i < nelemsp; i++)
			result->x[i] = DatumGetFloat4(elemsp[i]);
	}
	else if (ARR_ELEMTYPE(array) == NUMERICOID)
	{
		for (int i = 0; i < nelemsp; i++)
			result->x[i] = DatumGetFloat4(DirectFunctionCall1(numeric_float4, elemsp[i]));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("unsupported array type")));
	}

	/*
	 * Free allocation from deconstruct_array. Do not free individual elements
	 * when pass-by-reference since they point to original array.
	 */
	pfree(elemsp);

	/* Check elements */
	for (int i = 0; i < result->dim; i++)
		CheckElement(result->x[i]);

	PG_RETURN_POINTER(result);
}

/*
 * Convert vector to float4[]
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_to_float4);
Datum vector_to_float4(PG_FUNCTION_ARGS)
{
	Vector *vec = PG_GETARG_VECTOR_P(0);
	Datum *datums;
	ArrayType *result;

	datums = (Datum *)palloc(sizeof(Datum) * vec->dim);

	for (int i = 0; i < vec->dim; i++)
		datums[i] = Float4GetDatum(vec->x[i]);

	/* Use TYPALIGN_INT for float4 */
	result = construct_array(datums, vec->dim, FLOAT4OID, sizeof(float4), true, TYPALIGN_INT);

	pfree(datums);

	PG_RETURN_POINTER(result);
}

/*
 * Convert half vector to vector
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(halfvec_to_vector);
Datum halfvec_to_vector(PG_FUNCTION_ARGS)
{
	HalfVector *vec = PG_GETARG_HALFVEC_P(0);
	int32 typmod = PG_GETARG_INT32(1);
	Vector *result;

	CheckDim(vec->dim);
	CheckExpectedDim(typmod, vec->dim);

	result = InitVector(vec->dim);

	for (int i = 0; i < vec->dim; i++)
		result->x[i] = HalfToFloat4(vec->x[i]);

	PG_RETURN_POINTER(result);
}

VECTOR_TARGET_CLONES static float
VectorL2SquaredDistance(int dim, float *ax, float *bx)
{
	float distance = 0.0;

	/* Auto-vectorized */
	for (int i = 0; i < dim; i++)
	{
		float diff = ax[i] - bx[i];

		distance += diff * diff;
	}

	return distance;
}

/*
 * Get the L2 distance between vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(l2_distance);
Datum l2_distance(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8(sqrt((double)VectorL2SquaredDistance(a->dim, a->x, b->x)));
}

/*
 * Get the L2 squared distance between vectors
 * This saves a sqrt calculation
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_l2_squared_distance);
Datum vector_l2_squared_distance(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8((double)VectorL2SquaredDistance(a->dim, a->x, b->x));
}

VECTOR_TARGET_CLONES static float
VectorInnerProduct(int dim, float *ax, float *bx)
{
	float distance = 0.0;

	/* Auto-vectorized */
	for (int i = 0; i < dim; i++)
		distance += ax[i] * bx[i];

	return distance;
}

/*
 * Get the inner product of two vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(inner_product);
Datum inner_product(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8((double)VectorInnerProduct(a->dim, a->x, b->x));
}

/*
 * Get the negative inner product of two vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_negative_inner_product);
Datum vector_negative_inner_product(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8((double)-VectorInnerProduct(a->dim, a->x, b->x));
}

VECTOR_TARGET_CLONES static double
VectorCosineSimilarity(int dim, float *ax, float *bx)
{
	float similarity = 0.0;
	float norma = 0.0;
	float normb = 0.0;

	/* Auto-vectorized */
	for (int i = 0; i < dim; i++)
	{
		similarity += ax[i] * bx[i];
		norma += ax[i] * ax[i];
		normb += bx[i] * bx[i];
	}

	/* Use sqrt(a * b) over sqrt(a) * sqrt(b) */
	return (double)similarity / sqrt((double)norma * (double)normb);
}

/*
 * Get the cosine distance between two vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(cosine_distance);
Datum cosine_distance(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);
	double similarity;

	CheckDims(a, b);

	similarity = VectorCosineSimilarity(a->dim, a->x, b->x);

#ifdef _MSC_VER
	/* /fp:fast may not propagate NaN */
	if (isnan(similarity))
		PG_RETURN_FLOAT8(NAN);
#endif

	/* Keep in range */
	if (similarity > 1)
		similarity = 1.0;
	else if (similarity < -1)
		similarity = -1.0;

	PG_RETURN_FLOAT8(1.0 - similarity);
}

/*
 * Get the distance for spherical k-means
 * Currently uses angular distance since needs to satisfy triangle inequality
 * Assumes inputs are unit vectors (skips norm)
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_spherical_distance);
Datum vector_spherical_distance(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);
	double distance;

	CheckDims(a, b);

	distance = (double)VectorInnerProduct(a->dim, a->x, b->x);

	/* Prevent NaN with acos with loss of precision */
	if (distance > 1)
		distance = 1;
	else if (distance < -1)
		distance = -1;

	PG_RETURN_FLOAT8(acos(distance) / M_PI);
}

/* Does not require FMA, but keep logic simple */
VECTOR_TARGET_CLONES static float
VectorL1Distance(int dim, float *ax, float *bx)
{
	float distance = 0.0;

	/* Auto-vectorized */
	for (int i = 0; i < dim; i++)
		distance += fabsf(ax[i] - bx[i]);

	return distance;
}

/*
 * Get the L1 distance between two vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(l1_distance);
Datum l1_distance(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8((double)VectorL1Distance(a->dim, a->x, b->x));
}

/*
 * Get the dimensions of a vector
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_dims);
Datum vector_dims(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);

	PG_RETURN_INT32(a->dim);
}

/*
 * Get the L2 norm of a vector
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_norm);
Datum vector_norm(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	float *ax = a->x;
	double norm = 0.0;

	/* Auto-vectorized */
	for (int i = 0; i < a->dim; i++)
		norm += (double)ax[i] * (double)ax[i];

	PG_RETURN_FLOAT8(sqrt(norm));
}

/*
 * Normalize a vector with the L2 norm
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(l2_normalize);
Datum l2_normalize(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	float *ax = a->x;
	double norm = 0;
	Vector *result;
	float *rx;

	result = InitVector(a->dim);
	rx = result->x;

	/* Auto-vectorized */
	for (int i = 0; i < a->dim; i++)
		norm += (double)ax[i] * (double)ax[i];

	norm = sqrt(norm);

	/* Return zero vector for zero norm */
	if (norm > 0)
	{
		for (int i = 0; i < a->dim; i++)
			rx[i] = ax[i] / norm;

		/* Check for overflow */
		for (int i = 0; i < a->dim; i++)
		{
			if (isinf(rx[i]))
				float_overflow_error();
		}
	}

	PG_RETURN_POINTER(result);
}

/*
 * Add vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_add);
Datum vector_add(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);
	float *ax = a->x;
	float *bx = b->x;
	Vector *result;
	float *rx;

	CheckDims(a, b);

	result = InitVector(a->dim);
	rx = result->x;

	/* Auto-vectorized */
	for (int i = 0, imax = a->dim; i < imax; i++)
		rx[i] = ax[i] + bx[i];

	/* Check for overflow */
	for (int i = 0, imax = a->dim; i < imax; i++)
	{
		if (isinf(rx[i]))
			float_overflow_error();
	}

	PG_RETURN_POINTER(result);
}

/*
 * Subtract vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_sub);
Datum vector_sub(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);
	float *ax = a->x;
	float *bx = b->x;
	Vector *result;
	float *rx;

	CheckDims(a, b);

	result = InitVector(a->dim);
	rx = result->x;

	/* Auto-vectorized */
	for (int i = 0, imax = a->dim; i < imax; i++)
		rx[i] = ax[i] - bx[i];

	/* Check for overflow */
	for (int i = 0, imax = a->dim; i < imax; i++)
	{
		if (isinf(rx[i]))
			float_overflow_error();
	}

	PG_RETURN_POINTER(result);
}

/*
 * Multiply vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_mul);
Datum vector_mul(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);
	float *ax = a->x;
	float *bx = b->x;
	Vector *result;
	float *rx;

	CheckDims(a, b);

	result = InitVector(a->dim);
	rx = result->x;

	/* Auto-vectorized */
	for (int i = 0, imax = a->dim; i < imax; i++)
		rx[i] = ax[i] * bx[i];

	/* Check for overflow and underflow */
	for (int i = 0, imax = a->dim; i < imax; i++)
	{
		if (isinf(rx[i]))
			float_overflow_error();

		if (rx[i] == 0 && !(ax[i] == 0 || bx[i] == 0))
			float_underflow_error();
	}

	PG_RETURN_POINTER(result);
}

/*
 * Concatenate vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_concat);
Datum vector_concat(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);
	Vector *result;
	int dim = a->dim + b->dim;

	CheckDim(dim);
	result = InitVector(dim);

	for (int i = 0; i < a->dim; i++)
		result->x[i] = a->x[i];

	for (int i = 0; i < b->dim; i++)
		result->x[i + a->dim] = b->x[i];

	PG_RETURN_POINTER(result);
}

/*
 * Quantize a vector
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(binary_quantize);
Datum binary_quantize(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	float *ax = a->x;
	VarBit *result = InitBitVector(a->dim);
	unsigned char *rx = VARBITS(result);

	for (int i = 0; i < a->dim; i++)
		rx[i / 8] |= (ax[i] > 0) << (7 - (i % 8));

	PG_RETURN_VARBIT_P(result);
}

/*
 * Get a subvector
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(subvector);
Datum subvector(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	int32 start = PG_GETARG_INT32(1);
	int32 count = PG_GETARG_INT32(2);
	int32 end;
	float *ax = a->x;
	Vector *result;
	int dim;

	if (count < 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("vector must have at least 1 dimension")));

	/*
	 * Check if (start + count > a->dim), avoiding integer overflow. a->dim
	 * and count are both positive, so a->dim - count won't overflow.
	 */
	if (start > a->dim - count)
		end = a->dim + 1;
	else
		end = start + count;

	/* Indexing starts at 1, like substring */
	if (start < 1)
		start = 1;
	else if (start > a->dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("vector must have at least 1 dimension")));

	dim = end - start;
	CheckDim(dim);
	result = InitVector(dim);

	for (int i = 0; i < dim; i++)
		result->x[i] = ax[start - 1 + i];

	PG_RETURN_POINTER(result);
}

/*
 * Internal helper to compare vectors
 */
int vector_cmp_internal(Vector *a, Vector *b)
{
	int dim = Min(a->dim, b->dim);

	/* Check values before dimensions to be consistent with Postgres arrays */
	for (int i = 0; i < dim; i++)
	{
		if (a->x[i] < b->x[i])
			return -1;

		if (a->x[i] > b->x[i])
			return 1;
	}

	if (a->dim < b->dim)
		return -1;

	if (a->dim > b->dim)
		return 1;

	return 0;
}

/*
 * Less than
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_lt);
Datum vector_lt(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) < 0);
}

/*
 * Less than or equal
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_le);
Datum vector_le(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) <= 0);
}

/*
 * Equal
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_eq);
Datum vector_eq(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) == 0);
}

/*
 * Not equal
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_ne);
Datum vector_ne(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) != 0);
}

/*
 * Greater than or equal
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_ge);
Datum vector_ge(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) >= 0);
}

/*
 * Greater than
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_gt);
Datum vector_gt(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) > 0);
}

/*
 * Compare vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_cmp);
Datum vector_cmp(PG_FUNCTION_ARGS)
{
	Vector *a = PG_GETARG_VECTOR_P(0);
	Vector *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_INT32(vector_cmp_internal(a, b));
}

/*
 * Accumulate vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_accum);
Datum vector_accum(PG_FUNCTION_ARGS)
{
	ArrayType *statearray = PG_GETARG_ARRAYTYPE_P(0);
	Vector *newval = PG_GETARG_VECTOR_P(1);
	float8 *statevalues;
	int16 dim;
	bool newarr;
	float8 n;
	Datum *statedatums;
	float *x = newval->x;
	ArrayType *result;

	/* Check array before using */
	statevalues = CheckStateArray(statearray, "vector_accum");
	dim = STATE_DIMS(statearray);
	newarr = dim == 0;

	if (newarr)
		dim = newval->dim;
	else
		CheckExpectedDim(dim, newval->dim);

	n = statevalues[0] + 1.0;

	statedatums = CreateStateDatums(dim);
	statedatums[0] = Float8GetDatum(n);

	if (newarr)
	{
		for (int i = 0; i < dim; i++)
			statedatums[i + 1] = Float8GetDatum((double)x[i]);
	}
	else
	{
		for (int i = 0; i < dim; i++)
		{
			double v = statevalues[i + 1] + x[i];

			/* Check for overflow */
			if (isinf(v))
				float_overflow_error();

			statedatums[i + 1] = Float8GetDatum(v);
		}
	}

	/* Use float8 array like float4_accum */
	result = construct_array(statedatums, dim + 1,
							 FLOAT8OID,
							 sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

	pfree(statedatums);

	PG_RETURN_ARRAYTYPE_P(result);
}

/*
 * Combine vectors or half vectors (also used for halfvec_combine)
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_combine);
Datum vector_combine(PG_FUNCTION_ARGS)
{
	/* Must also update parameters of halfvec_combine if modifying */
	ArrayType *statearray1 = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType *statearray2 = PG_GETARG_ARRAYTYPE_P(1);
	float8 *statevalues1;
	float8 *statevalues2;
	float8 n;
	float8 n1;
	float8 n2;
	int16 dim;
	Datum *statedatums;
	ArrayType *result;

	/* Check arrays before using */
	statevalues1 = CheckStateArray(statearray1, "vector_combine");
	statevalues2 = CheckStateArray(statearray2, "vector_combine");

	n1 = statevalues1[0];
	n2 = statevalues2[0];

	if (n1 == 0.0)
	{
		n = n2;
		dim = STATE_DIMS(statearray2);
		statedatums = CreateStateDatums(dim);
		for (int i = 1; i <= dim; i++)
			statedatums[i] = Float8GetDatum(statevalues2[i]);
	}
	else if (n2 == 0.0)
	{
		n = n1;
		dim = STATE_DIMS(statearray1);
		statedatums = CreateStateDatums(dim);
		for (int i = 1; i <= dim; i++)
			statedatums[i] = Float8GetDatum(statevalues1[i]);
	}
	else
	{
		n = n1 + n2;
		dim = STATE_DIMS(statearray1);
		CheckExpectedDim(dim, STATE_DIMS(statearray2));
		statedatums = CreateStateDatums(dim);
		for (int i = 1; i <= dim; i++)
		{
			double v = statevalues1[i] + statevalues2[i];

			/* Check for overflow */
			if (isinf(v))
				float_overflow_error();

			statedatums[i] = Float8GetDatum(v);
		}
	}

	statedatums[0] = Float8GetDatum(n);

	result = construct_array(statedatums, dim + 1,
							 FLOAT8OID,
							 sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

	pfree(statedatums);

	PG_RETURN_ARRAYTYPE_P(result);
}

/*
 * Average vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_avg);
Datum vector_avg(PG_FUNCTION_ARGS)
{
	ArrayType *statearray = PG_GETARG_ARRAYTYPE_P(0);
	float8 *statevalues;
	float8 n;
	uint16 dim;
	Vector *result;

	/* Check array before using */
	statevalues = CheckStateArray(statearray, "vector_avg");
	n = statevalues[0];

	/* SQL defines AVG of no values to be NULL */
	if (n == 0.0)
		PG_RETURN_NULL();

	/* Create vector */
	dim = STATE_DIMS(statearray);
	CheckDim(dim);
	result = InitVector(dim);
	for (int i = 0; i < dim; i++)
	{
		result->x[i] = statevalues[i + 1] / n;
		CheckElement(result->x[i]);
	}

	PG_RETURN_POINTER(result);
}

/*
 * Convert sparse vector to dense vector
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(sparsevec_to_vector);
Datum sparsevec_to_vector(PG_FUNCTION_ARGS)
{
	SparseVector *svec = PG_GETARG_SPARSEVEC_P(0);
	int32 typmod = PG_GETARG_INT32(1);
	Vector *result;
	int dim = svec->dim;
	float *values = SPARSEVEC_VALUES(svec);

	CheckDim(dim);
	CheckExpectedDim(typmod, dim);

	result = InitVector(dim);
	for (int i = 0; i < svec->nnz; i++)
		result->x[svec->indices[i]] = values[i];

	PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(hello_world);
Datum hello_world(PG_FUNCTION_ARGS)
{
	// 将 C 字符串转换为 PostgreSQL 的文本类型
	text *result = cstring_to_text("Hello, World!");

	// 返回文本
	PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(load_fbin_to_pgvector);
Datum load_fbin_to_pgvector(PG_FUNCTION_ARGS)
{
	text *filepath_text = PG_GETARG_TEXT_PP(0);
	char *filepath = text_to_cstring(filepath_text);
	FILE *file;
	int dim = 128;
	float *buffer;
	int count = 0;
	char create_table_sql[512];
	int current_dim;
	bool beginning = true;
	StringInfoData single_insert_sql; // 改为单条插入语句缓冲区

	elog(INFO, "Starting FBIN import from: %s", filepath);

	/* 打开文件并检查有效性 */
	if ((file = fopen(filepath, "rb")) == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("Cannot open file: %s", filepath)));

	/* 检查pgvector扩展 */
	SPI_connect();
	if (SPI_execute("SELECT 1 FROM pg_type WHERE typname = 'vector'", true, 0) != SPI_OK_SELECT)
	{
		SPI_finish();
		fclose(file);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("pgvector extension not installed")));
	}
	if (SPI_processed == 0)
	{
		SPI_finish();
		fclose(file);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("vector type not found")));
	}

	/* 创建表（如果不存在） */
	snprintf(create_table_sql, sizeof(create_table_sql),
			 "CREATE TABLE IF NOT EXISTS vectors ("
			 "id SERIAL PRIMARY KEY,"
			 "embedding vector(128),neighbors integer[])");

	if (SPI_execute(create_table_sql, false, 0) != SPI_OK_UTILITY)
	{
		SPI_finish();
		fclose(file);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("Table creation failed: %s", SPI_result_code_string(SPI_result))));
	}

	/* 准备单条插入语句模板 */
	initStringInfo(&single_insert_sql);
	buffer = (float *)palloc(dim * sizeof(float));

	// 代码使用 while (!feof(file)) 循环读取数据，但 feof() 在读取失败后才会返回 true。
	// 这可能导致最后一次循环尝试读取不存在的数据，触发 n != dim 错误。
	// 尤其在文件大小刚好为整数个向量时，最后一次循环会多执行一次，导致错误。
	// 改成使用 feof() 作为循环条件，可以避免这个问题。
	/* 读取数据并逐条插入 */
	while (true)
	{
		size_t n_header = fread(&current_dim, sizeof(int), 1, file);
		if (n_header == 0)
		{
			if (feof(file))
				break; // 正常结束
			else
			{
				// 处理读取错误
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("Reading Incomplete vector data header at position %d", count)));
			}
		}

		// 转换字节序（fvecs 是小端序）
		// current_dim = ntohl(current_dim); // 若文件是大端序需保留此行
		if (beginning)
		{
			beginning = false;
			elog(INFO, "Detected vector dimension: %d", current_dim);
		}

		if (current_dim != 128)
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("Dimension mismatch: expected %d, got %d at position %d", dim, current_dim, count)));
		}

		// 读取向量数据
		size_t n = fread(buffer, sizeof(float), dim, file);
		if (n != dim)
		{
			elog(INFO, "n = %ld, dim = %d", n, dim);
			pfree(buffer);
			SPI_finish();
			fclose(file);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("Incomplete vector data at position %d", count)));
		}

		/* 构建单条INSERT语句 */
		resetStringInfo(&single_insert_sql); // 清空缓冲区
		// elog(INFO, "检查缓冲区：%s", single_insert_sql.data);
		appendStringInfoString(&single_insert_sql, "INSERT INTO vectors (embedding) VALUES ('[");

		// 构建向量字符串
		for (int i = 0; i < dim; i++)
		{
			if (i > 0)
				appendStringInfoString(&single_insert_sql, ", ");
			appendStringInfo(&single_insert_sql, "%.9g", buffer[i]);
		}

		appendStringInfoString(&single_insert_sql, "]'::vector)");

		/* 执行单条插入 */
		if (SPI_execute(single_insert_sql.data, false, 0) != SPI_OK_INSERT)
		{
			pfree(buffer);
			SPI_finish();
			fclose(file);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("Insert failed at position %d: %s", count, SPI_result_code_string(SPI_result))));
		}

		count++;

		/* 可选：每插入N条打印进度 */
		if (count % 1000 == 0)
		{
			elog(INFO, "Inserted %d records", count);
		}
	}

	/* 清理资源 */
	pfree(buffer);
	pfree(single_insert_sql.data); // 释放语句缓冲区
	SPI_finish();
	fclose(file);

	elog(INFO, "Successfully imported %d vectors", count);
	PG_RETURN_INT32(count);
}

PG_FUNCTION_INFO_V1(test_vamana);
Datum test_vamana(PG_FUNCTION_ARGS)
{
	elog(INFO, "Hello, Vamana!");
	size_t npt_val = 5;
	// 维度
	size_t dim_val = 5;
	// 切片大小
	size_t slice_size = 0;

	// 加载向量数据
	float *storage = NULL;
	// load_vector_data("vectors", "embedding", &npt_val, &dim_val);
	// elog(INFO,"npt_val = %d, dim_val = %d", npt_val, dim_val);
	//  gen_random_slice(aa, npt_val, dim_val, 0.01, &storage, &slice_size);
	const char *dataFile = "/mnt/c/dev/repository/graduation/my_pgvector/pgvector/data/siftsmall_learn.fbin";
	const char *indexFile = "/mnt/c/dev/repository/graduation/my_pgvector/pgvector/data/test";
	// L=50,R=64,C=200
	const char *buildParams = "10 20 200 1 1";
	enum diskann_metric_t metric = DISKANN_L2; // 假设使用 L2 作为度量方式
	int use_opq = false;					   // 启用 OPQ
	const char *codebookPrefix = "/path/to/codebook";

	int status = build_disk_index(dataFile, indexFile, buildParams, metric, use_opq, codebookPrefix, npt_val, dim_val);
	PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(test_neighbours);
Datum test_neighbours(PG_FUNCTION_ARGS)
{
	uint32_t R = 5;
	uint32_t num_point = 10;

	NewVector *result = (NewVector *)calloc(1, sizeof(NewVector));
	generate_random_neighbors_for_vector_empty(result, (size_t)num_point, (size_t)R);
	// my2d_vector_init(&result, 1000, 3);
	for (int i = 0; i < num_point; i++)
	{
		NewVector *cur_vec = (NewVector *)new_vector_get(result, i);
		for (int j = 0; j < cur_vec->size; j++)
		{
			uint32_t *val = new_vector_get(cur_vec, j);
			// elog(INFO, "posi:%d,posj:%d,value:%d", i, j, *val);
		}
	}
	elog(INFO, "element size:%d", result->elem_size);
	elog(INFO, "element size:%d", result->size);
	PG_RETURN_NULL();
	// NewVector *cur_vec = (NewVector *)new_vector_get(result, 0);

	// // resize bigger data not changed
	// size_t new_sizeee = 9;
	// new_vector_reserve(cur_vec, new_sizeee);
	// for (uint32_t j = 0; j < cur_vec->size; j++)
	// {
	// 	uint32_t *val = new_vector_get(cur_vec, j);
	// 	elog(INFO, " resize bigger data not changed pos:%d,value:%d", j, *val);
	// }

	// // resize smaller
	// new_vector_reserve(cur_vec, 3);
	// NewVector *new_vec = (NewVector *)palloc(sizeof(NewVector));
	// new_vector_init_with_capacity(new_vec, sizeof(uint32_t), 10);
	// for (uint32_t i = 0; i < new_vec->capacity; i++)
	// {
	// 	new_vector_push_back(new_vec, &i);
	// }
	// elog(INFO, "new_vec size:%d", new_vec->size);
	// for (uint32_t j = 0; j < new_vec->size; j++)
	// {
	// 	uint32_t *val = new_vector_get(new_vec, j);
	// 	elog(INFO, "new_vec pos:%d,value:%d", j, *val);
	// }
	// memcpy(cur_vec->data, new_vec->data, 9 * sizeof(uint32_t));
	// memcpy((char *)cur_vec->data, (char *)new_vec->data, 9 * sizeof(uint32_t));
	// for (uint32_t j = 0; j < cur_vec->size; j++)
	// {
	// 	uint32_t *val = new_vector_get(cur_vec, j);
	// 	elog(INFO, "pos:%d,value:%d", j, *val);
	// }

	// new_vector_reserve(cur_vec, new_vec->size);
	// new_vector_resize(cur_vec, new_vec->size);
	// memcpy((char *)cur_vec->data, (char *)new_vec->data, 9 * sizeof(uint32_t));
	// for (int j = 0; j < cur_vec->size; j++)
	// {
	// 	uint32_t *val = new_vector_get(cur_vec, j);
	// 	elog(INFO, "pos:%d,value:%d", j, *val);
	// }

	// new_vector_free(new_vec);

	PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(test_func);
Datum test_func(PG_FUNCTION_ARGS)
{
	// NewVector *result = (NewVector *)palloc(sizeof(NewVector));
	// generate_random_neighbors_for_vector(result, 3, 2);
	// save_neighbors_to_disk(result);

	// NewVector *des_neighbors = (NewVector *)palloc(sizeof(NewVector));
	// MyVector *neighbors = (MyVector *)palloc(sizeof(MyVector));
	// new_vector_init(des_neighbors, sizeof(uint32_t));
	// vector_init(neighbors);
	// vector_push_back(neighbors, 1);
	// vector_push_back(neighbors, 2);
	// vector_push_back(neighbors, 3);
	// new_vector_reserve(des_neighbors, neighbors->size);
	// new_vector_resize(des_neighbors, neighbors->size);
	// memcpy((char *)des_neighbors->data, (char *)neighbors->data, neighbors->size * sizeof(uint32_t));
	// for(int i = 0; i < des_neighbors->size; i++)
	// {
	// 	uint32_t *val = new_vector_get(des_neighbors, i);
	// 	elog(INFO, "pos:%d,value:%d", i, *val);
	// }
	// bool is_ok = create_index_table("aaa", "vectors", 2);

	// NewVector *target = (NewVector *)palloc(sizeof(NewVector));
	// new_vector_init(target, sizeof(uint32_t));
	// uint32_t val = 1;
	// uint32_t val2 = 2;
	// uint32_t val3 = 3;
	// new_vector_push_back(target, &val);
	// new_vector_push_back(target, &val2);
	// new_vector_push_back(target, &val3);
	// NewVector *res = (NewVector *)palloc(sizeof(NewVector));
	// new_vector_init(res, sizeof(VectorCache));
	// get_vectors_and_neighbors("vectors_index_table", res, target);
	// elog(INFO, "Hello, Func!,res_size:%d", res->size);
	// for (uint32_t i = 0; i < res->size; i++)
	// {
	// 	VectorCache *cur_cache = (VectorCache *)new_vector_get(res, i);
	// 	elog(INFO, "cur_cache->id:%d", cur_cache->vector_id);
	// 	elog(INFO, "res_size:%d", cur_cache->neighbor_count);
	// 	// elog(INFO, "cur_cache->neighbors->size:%d", cur_cache->neighbors->size);
	// 	for (uint32_t j = 0; j < cur_cache->neighbor_count; j++)
	// 	{
	// 		elog(INFO, "pos:%d,value:%d", j, cur_cache->neighbors[j]);
	// 	}
	// 	char* buf;
	// 	PrintVector(buf, cur_cache->vector);
	// }

	// 测试新写的搜索函数
	// Vector *target = InitVector(128);
	// // get_vector_in_database("vectors_index_table", 1, target);

	// for (uint32_t i = 0; i < 128; i++)
	// {
	// 	target->x[i] = i % 64;
	// }
	// char *printVec;
	// PrintVector(printVec, target);
	// elog(INFO, "target:%s", printVec);
	// search_k_nearest_neighbors("vectors_index_table", 30, 10, target, 128);

	// 测试构建索引

	PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(test_recall);
Datum test_recall(PG_FUNCTION_ARGS)
{
	// 加载真值集
	File *truth_file;
	const char *truthfilepath = "/mnt/c/dev/repository/graduation/my_pgvector/pgvector/data/siftsmall_groundtruth.ivecs";
	if ((truth_file = fopen(truthfilepath, "rb")) == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("Cannot open file: %s", truthfilepath)));
	int32_t *ids = (int32_t *)malloc(100 * 10 * sizeof(int32_t));
	int32_t *new_buffer = (int32_t *)malloc(100 * sizeof(int32_t));
	// int32_t *tmp = (int32_t *)palloc(10 * sizeof(int32_t));
	int32_t num = 0;
	int32_t count_query = 0;

	// recall
	float recall_sum = 0.0;
	int query_total = 100; // 总查询数量
	int k = 10;
	while (true)
	{
		size_t n_header = fread(&num, sizeof(int32_t), 1, truth_file);
		if (n_header == 0)
		{
			if (feof(truth_file))
			{
				break;
			}
			else
			{
				elog(ERROR, "error reading");
			}
		}
		size_t n = fread(new_buffer, sizeof(int32_t), 100, truth_file);
		// memcpy函数使用注意点 在dest指针中使用加减法时，注意指针的类型
		memcpy(ids + count_query * 10, new_buffer, 10 * sizeof(int32_t));
		count_query++;
		elog(INFO, "count_query:%d", count_query);
	}

	for (int i = 0; i < 10; i++)
	{
		elog(INFO, "PRINT:%d", ids[i * 10]);
	}

	// 加载query数据集
	float *buffer;
	const char *filepath = "/mnt/c/dev/repository/graduation/my_pgvector/pgvector/data/siftsmall_query.fvecs";
	FILE *file;
	uint32_t dim = 128;

	if ((file = fopen(filepath, "rb")) == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("Cannot open file: %s", filepath)));
	uint32_t ndims_i32;
	size_t expected_bytes = 0, bytes_read = 0;
	// 读取dim
	// 读取数据
	buffer = (float *)malloc(dim * sizeof(float));
	int32_t count = 0;
	while (true)
	{

		size_t n_header = fread(&dim, sizeof(uint32_t), 1, file);
		if (n_header == 0)
		{
			if (feof(file))
				break; // 正常结束
			else
			{
				// 处理读取错误
				elog(ERROR, "error reading");
			}
		}
		// elog(INFO, "count:%d", count);
		//  读取数据
		elog(INFO, "truth value:%d,%d,%d,%d,%d", ids[count * 10], ids[count * 10 + 1], ids[count * 10 + 2], ids[count * 10 + 3], ids[count * 10 + 4]);
		elog(INFO, "truth value:%d,%d,%d,%d,%d", ids[count * 10 + 5], ids[count * 10 + 6], ids[count * 10 + 7], ids[count * 10 + 8], ids[count * 10 + 9]);
		bytes_read += 1 * sizeof(uint32_t);
		size_t n = fread(buffer, sizeof(float), dim, file);
		// 将数组转化为vector
		Vector *vec = InitVector(dim);
		for (int i = 0; i < dim; i++)
		{
			vec->x[i] = buffer[i];
		}
		MyPrintVector(vec);

		bytes_read += n * sizeof(float);

		// 进行查询
		uint32_t init_id5 = 2176;  // 0-9999
		uint32_t init_id4 = 3752; // 0-9999
		uint32_t init_id3 = 2781; // 0-9999
		uint32_t init_id2 = 2707; // 0-9999
		uint32_t init_id = 9843; // 0-9999
		const char *table_name = "vectors_index_table";
		NewVector *init_ids = (NewVector *)palloc(sizeof(NewVector));
		new_vector_init_with_capacity(init_ids, sizeof(uint32_t), 10);
		new_vector_push_back(init_ids, &init_id);
		new_vector_push_back(init_ids, &init_id2);
		new_vector_push_back(init_ids, &init_id3);
		new_vector_push_back(init_ids, &init_id4);
		new_vector_push_back(init_ids, &init_id5);
		// uint32_t init_id = 2176; // 0-9999

		// uint32_t k = 20;
		NewVector *target_nbrs = new_search_k_nearest_neighbors(table_name, init_ids, k, vec, 10000);
		char result[1024]; // 足够大的输出缓冲区
		new_vector_to_string(target_nbrs, result, sizeof(result));
		elog(INFO, "result:%s", result);

		// 计算召回率
		int hit = 0;
		int32_t *truth_topk = &ids[count * k];
		count++;
		// 遍历搜索结果，统计有多少在真值集合里
		for (uint32_t i = 0; i < target_nbrs->size; i++)
		{
			uint32_t *pred_id = new_vector_get(target_nbrs, i);
			for (int j = 0; j < k; j++)
			{
				if (*pred_id == truth_topk[j])
				{
					hit++;
					break; // 找到后退出内层循环，避免重复计数
				}
			}
		}

		// 本次查询的召回率
		float recall = (float)hit / (float)k;
		elog(INFO, "Query %d recall: %.2f", count, recall);
		recall_sum += recall;
	}

	float avg_recall = recall_sum / query_total;
	elog(INFO, "Average recall: %.4f", avg_recall);

	free(ids);
	free(buffer);
	free(new_buffer);
	fclose(file);
	fclose(truth_file);
	// 对比结果 计算召回率
	PG_RETURN_NULL();
}

static float my_l2_distance(const float *a, const float *b, int dim)
{
	float sum = 0.0f;
	for (int i = 0; i < dim; i++)
	{
		float diff = a[i] - b[i];
		sum += diff * diff;
	}
	return (float)sum;
}

PG_FUNCTION_INFO_V1(check_dist);
Datum check_dist(PG_FUNCTION_ARGS)
{
	// 参数设置
	int dim = 128;
	int num_queries = 100;
	int base_vector_num = 10000;
	int top_k = 100;

	const char *query_path = "/mnt/c/dev/repository/graduation/my_pgvector/pgvector/data/siftsmall_query.fvecs";
	const char *truth_path = "/mnt/c/dev/repository/graduation/my_pgvector/pgvector/data/siftsmall_groundtruth.ivecs";
	const char *base_path = "/mnt/c/dev/repository/graduation/my_pgvector/pgvector/data/siftsmall_base.fvecs";
	const char *csv_output_path = "/tmp/true_distances.csv";

	// 打开文件
	FILE *query_file = fopen(query_path, "rb");
	FILE *truth_file = fopen(truth_path, "rb");
	FILE *base_file = fopen(base_path, "rb");
	FILE *csv_file = fopen(csv_output_path, "w");

	if (!query_file || !truth_file || !base_file || !csv_file)
		ereport(ERROR, (errmsg("无法打开数据文件")));

	// 分配内存
	float *query_vectors = malloc(num_queries * dim * sizeof(float));
	float *base_vectors = malloc(base_vector_num * dim * sizeof(float));
	int32_t *truth_ids = malloc(num_queries * top_k * sizeof(int32_t));
	if (!query_vectors || !base_vectors || !truth_ids)
		ereport(ERROR, (errmsg("内存分配失败")));

	// 读取 query 向量
	for (int i = 0; i < num_queries; i++)
	{
		int vec_dim = 0;
		fread(&vec_dim, sizeof(int32_t), 1, query_file);
		if (vec_dim != dim)
			ereport(ERROR, (errmsg("query 维度不符: %d", vec_dim)));
		fread(query_vectors + i * dim, sizeof(float), dim, query_file);
	}

	// 读取 base 向量
	for (int i = 0; i < base_vector_num; i++)
	{
		int vec_dim = 0;
		fread(&vec_dim, sizeof(int32_t), 1, base_file);
		if (vec_dim != dim)
			ereport(ERROR, (errmsg("base 向量维度不符: %d", vec_dim)));
		fread(base_vectors + i * dim, sizeof(float), dim, base_file);
	}

	// 读取 ground truth
	for (int i = 0; i < num_queries; i++)
	{
		int count = 0;
		fread(&count, sizeof(int32_t), 1, truth_file);
		if (count != top_k)
			ereport(ERROR, (errmsg("ground truth k != %d: %d", top_k, count)));
		fread(truth_ids + i * top_k, sizeof(int32_t), top_k, truth_file);
	}

	// 写入 CSV 标题
	fprintf(csv_file, "query_id");
	for (int j = 0; j < top_k; j++)
	{
		fprintf(csv_file, ",neighbor_%d_dist", j);
	}
	fprintf(csv_file, "\n");

	// 主循环：每个 query 计算 top-k 真值邻居距离
	for (int q = 0; q < num_queries; q++)
	{
		float *query_vec = query_vectors + q * dim;

		// 打印 query 向量
		char vec_str[2048] = {0};
		strcat(vec_str, "Query vector: [");
		for (int d = 0; d < dim; d++)
		{
			char buf[32];
			snprintf(buf, sizeof(buf), "%.3f%s", query_vec[d], (d < dim - 1) ? ", " : "]");
			strcat(vec_str, buf);
		}
		elog(INFO, "Query %d - %s", q, vec_str);

		// 写入 CSV
		fprintf(csv_file, "%d", q);

		for (int j = 0; j < top_k; j++)
		{
			int nbr_id = truth_ids[q * top_k + j];
			float *nbr_vec = base_vectors + nbr_id * dim;
			float dist = my_l2_distance(query_vec, nbr_vec, dim);
			fprintf(csv_file, ",%.4f", dist);

			if (q < 2)
			{
				elog(INFO, "  Neighbor %d (id=%d), dist=%.4f", j, nbr_id, dist);
			}
		}

		fprintf(csv_file, "\n");
	}

	// 释放资源
	fclose(query_file);
	fclose(base_file);
	fclose(truth_file);
	fclose(csv_file);
	free(query_vectors);
	free(base_vectors);
	free(truth_ids);

	elog(INFO, "真值距离计算完毕，CSV 写入: %s", csv_output_path);
	PG_RETURN_NULL();
}