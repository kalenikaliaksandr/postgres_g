#include "postgres.h"
#include "fmgr.h"
#include "access/hash.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/int8.h"
#include "utils/nabstime.h"
#include "utils/timestamp.h"
#include "utils/date.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

#define	NULLHASHVALUE		(-2147483647)

#define	FULLEQ_FUNC(type, cmpfunc, hashfunc)			\
PG_FUNCTION_INFO_V1( isfulleq_##type );					\
Datum	isfulleq_##type(PG_FUNCTION_ARGS);				\
Datum													\
isfulleq_##type(PG_FUNCTION_ARGS) {						\
	if ( PG_ARGISNULL(0) && PG_ARGISNULL(1) )			\
		PG_RETURN_BOOL(true);							\
	else if ( PG_ARGISNULL(0) || PG_ARGISNULL(1) )		\
		PG_RETURN_BOOL(false);							\
														\
	PG_RETURN_DATUM( DirectFunctionCall2( cmpfunc,		\
			PG_GETARG_DATUM(0),							\
			PG_GETARG_DATUM(1)							\
	) );												\
}														\
														\
PG_FUNCTION_INFO_V1( fullhash_##type );					\
Datum	fullhash_##type(PG_FUNCTION_ARGS);				\
Datum													\
fullhash_##type(PG_FUNCTION_ARGS) {						\
	if ( PG_ARGISNULL(0) )								\
		PG_RETURN_INT32(NULLHASHVALUE);					\
														\
	PG_RETURN_DATUM( DirectFunctionCall1( hashfunc,		\
			PG_GETARG_DATUM(0)							\
	) );												\
}


static Datum
hashint2vector(PG_FUNCTION_ARGS)
{
	int2vector *key = (int2vector *) PG_GETARG_POINTER(0);

	return hash_any((unsigned char *) key->values, key->dim1 * sizeof(int16));
}

/*
 * We don't have a complete set of int2vector support routines,
 * but we need int2vectoreq for catcache indexing.
 */
static Datum
int2vectoreq(PG_FUNCTION_ARGS)
{
	int2vector *a = (int2vector *) PG_GETARG_POINTER(0);
	int2vector *b = (int2vector *) PG_GETARG_POINTER(1);

	if (a->dim1 != b->dim1)
		PG_RETURN_BOOL(false);
	PG_RETURN_BOOL(memcmp(a->values, b->values, a->dim1 * sizeof(int16)) == 0);
}


FULLEQ_FUNC( bool        , booleq         , hashchar       );
FULLEQ_FUNC( bytea       , byteaeq        , hashvarlena    );
FULLEQ_FUNC( char        , chareq         , hashchar       );
FULLEQ_FUNC( name        , nameeq         , hashname       );
FULLEQ_FUNC( int8        , int8eq         , hashint8       );
FULLEQ_FUNC( int2        , int2eq         , hashint2       );
FULLEQ_FUNC( int4        , int4eq         , hashint4       );
FULLEQ_FUNC( text        , texteq         , hashtext       );
FULLEQ_FUNC( oid         , oideq          , hashoid        );
FULLEQ_FUNC( xid         , xideq          , hashint4       );
FULLEQ_FUNC( cid         , cideq          , hashint4       );
FULLEQ_FUNC( oidvector   , oidvectoreq    , hashoidvector  );
FULLEQ_FUNC( float4      , float4eq       , hashfloat4     );
FULLEQ_FUNC( float8      , float8eq       , hashfloat8     );
FULLEQ_FUNC( abstime     , abstimeeq      , hashint4       );
FULLEQ_FUNC( reltime     , reltimeeq      , hashint4       );
FULLEQ_FUNC( macaddr     , macaddr_eq     , hashmacaddr    );
FULLEQ_FUNC( inet        , network_eq     , hashinet       );
FULLEQ_FUNC( cidr        , network_eq     , hashinet       );
FULLEQ_FUNC( varchar     , texteq         , hashtext       );
FULLEQ_FUNC( date        , date_eq        , hashint4       );
FULLEQ_FUNC( time        , time_eq        , hashfloat8     );
FULLEQ_FUNC( timestamp   , timestamp_eq   , hashfloat8     );
FULLEQ_FUNC( timestamptz , timestamp_eq   , hashfloat8     );
FULLEQ_FUNC( interval    , interval_eq    , interval_hash  );
FULLEQ_FUNC( timetz      , timetz_eq      , timetz_hash    );

/*
 * v10 drop * support for int2vector equality and hash operator in commit
 * 5c80642aa8de8393b08cd3cbf612b325cedd98dc, but for compatibility
 * we still add this operators
 */
FULLEQ_FUNC( int2vector  , int2vectoreq   , hashint2vector );
