#include "postgres.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/typcache.h"
#include "utils/lsyscache.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "catalog/pg_type.h"
#include "common/int.h"

#include <string.h>

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(array_contains);
PG_FUNCTION_INFO_V1(array_contains_all);
PG_FUNCTION_INFO_V1(array_find_all);
PG_FUNCTION_INFO_V1(array_filter);
PG_FUNCTION_INFO_V1(array_reject);
PG_FUNCTION_INFO_V1(array_partition);
PG_FUNCTION_INFO_V1(array_index_of);
PG_FUNCTION_INFO_V1(array_last_index_of);

typedef enum {
    EQ_OP,
    N_EQ_OP,
    GR_OP,
    EQ_GR_OP,
    LE_OP,
    EQ_LE_OP,
    MOD_OP,
    NONE_OP
} predicate_op;

typedef struct {
    predicate_op op;
    Oid obj_type;
    Datum obj1;
    Datum obj2;
} predicate;

static int32 compare(Oid type, Datum obj1, Datum obj2, Oid collation)
{
    int32 cmp;

    switch (type)
    {
    case INT2OID:
        cmp = DatumGetInt32(DirectFunctionCall2(btint2cmp, obj1, obj2));
        break;

    case INT4OID:
        cmp = DatumGetInt32(DirectFunctionCall2(btint4cmp, obj1, obj2));
        break;

    case INT8OID:
        cmp = DatumGetInt32(DirectFunctionCall2(btint8cmp, obj1, obj2));
        break;

    case FLOAT4OID:
        cmp = DatumGetInt32(DirectFunctionCall2(btfloat4cmp, obj1, obj2));
        break;

    case FLOAT8OID:
        cmp = DatumGetInt32(DirectFunctionCall2(btfloat8cmp, obj1, obj2));
        break;

    case NUMERICOID:
        cmp = DatumGetInt32(DirectFunctionCall2(numeric_cmp, obj1, obj2));
        break;

    case TEXTOID:
        cmp = DatumGetInt32(DirectFunctionCall2Coll(bttextcmp, collation, obj1, obj2));
        break;

    case VARCHAROID:
        cmp = DatumGetInt32(DirectFunctionCall2Coll(bttextcmp, collation, obj1, obj2));
        break;

    case BPCHAROID:
        cmp = DatumGetInt32(DirectFunctionCall2Coll(bpcharcmp, collation, obj1, obj2));
        break;

    default:
        ereport(ERROR, errmsg("unsupported type for comparison"));
    }
    
    return cmp;
}

static Datum parse_item(const char *data, int len, int* pos, Oid item_type)
{
    int item_len = 0;
    Datum item;
    bool is_float = 0;
    if (data[(*pos)] == '"')
    {
        if (!(item_type == TEXTOID || item_type == VARCHAROID || item_type == BPCHAROID))
            ereport(ERROR, errmsg("incompatible types for comparison"));
        
        (*pos)++;
        while ((*pos) + item_len < len && data[(*pos) + item_len] != '"')
            item_len++;
        if ((*pos) + item_len >= len)
            ereport(ERROR, errmsg("unsupported predicate"));

        char* buf = palloc((item_len+1)*sizeof(char));
        memcpy(buf, data + (*pos), item_len*sizeof(char));
        buf[item_len] = '\0';

        switch (item_type) {
        case TEXTOID:
            item = DirectFunctionCall1(textin, CStringGetDatum(buf));
            break;

        case VARCHAROID:
            item = DirectFunctionCall1(textin, CStringGetDatum(buf));
            break;

        case BPCHAROID:
            item = DirectFunctionCall3(bpcharin, CStringGetDatum(buf), ObjectIdGetDatum(BPCHAROID), Int32GetDatum(-1));
            break;
        default:
            ereport(ERROR, errmsg("unsupported type for constant"));
        }

        (*pos) += item_len + 1;

        pfree(buf);
    }
    else
    {
        if (!(item_type == INT2OID || item_type == INT4OID || item_type == INT8OID ||
            item_type == FLOAT4OID || item_type == FLOAT8OID || item_type == NUMERICOID))
            ereport(ERROR, errmsg("incompatible types for comparison"));
        
        if ((*pos) + item_len < len && data[(*pos) + item_len] == '-')
            item_len++;
        
        while ((*pos) + item_len < len && ((data[(*pos) + item_len] >= '0' && data[(*pos) + item_len] <= '9') ||
            data[(*pos) + item_len] == '.'))
        {
            if (data[(*pos) + item_len] == '.')
            {
                if (is_float) ereport(ERROR, errmsg("unsupported predicate"));
                is_float = 1;
            }
            item_len++;
        }

        if (item_len == 0)
            ereport(ERROR, errmsg("unsupported predicate"));

        if (item_len == 1 && data[(*pos)] == '-')
            ereport(ERROR, errmsg("unsupported predicate"));

        if (is_float && (item_type == INT2OID || item_type == INT4OID || item_type == INT8OID))
            ereport(ERROR, errmsg("incompatible types for comparison"));

        char* buf = palloc((item_len+1)*sizeof(char));
        memcpy(buf, data + (*pos), item_len*sizeof(char));
        buf[item_len] = '\0';

            switch (item_type)
            {
            case INT2OID:
                item = Int16GetDatum((int16) pg_strtoint16(buf));
                break;
            case INT4OID:
                item = Int32GetDatum((int32) pg_strtoint32(buf));
                break;
            case INT8OID:
                item = Int64GetDatum((int64) pg_strtoint64(buf));
                break;
            case FLOAT4OID:
                item = DirectFunctionCall1(float4in, CStringGetDatum(buf));
                break;
            case FLOAT8OID:
                item = DirectFunctionCall1(float8in, CStringGetDatum(buf));
                break;
            case NUMERICOID: 
                item = DirectFunctionCall3(numeric_in, CStringGetDatum(buf), ObjectIdGetDatum(NUMERICOID), Int32GetDatum(-1));
                break;
            default:
                ereport(ERROR, errmsg("unsupported type for constant"));
            }

        (*pos) += item_len;

        pfree(buf);
    }

    return item;
}

static predicate_op parse_operation(const char *data, int len, int* pos)
{
    if ((*pos) >= len)
    {
        return NONE_OP;
    }
    switch (data[(*pos)])
    {
    case '=':
        if ((*pos) + 1 < len && data[(*pos) + 1] == '=')
            (*pos)++;
        (*pos)++;
        return EQ_OP;
    case '!':
        if ((*pos) + 1 < len && data[(*pos) + 1] == '=')
        {
            (*pos)+=2;
            return N_EQ_OP;
        }
        (*pos)++;
        ereport(ERROR, errmsg("unsupported predicate"));
    case '>':
        if ((*pos) + 1 < len && data[(*pos) + 1] == '=')
        {
            (*pos)+=2;
            return EQ_GR_OP;
        }
        (*pos)++;
        return GR_OP;
    case '<':
        if ((*pos) + 1 < len && data[(*pos) + 1] == '=')
        {
            (*pos)+=2;
            return EQ_LE_OP;
        }
        (*pos)++;
        return LE_OP;
    case '%':
        (*pos)++;
        return MOD_OP;
    default:
        ereport(ERROR, errmsg("unsupported predicate"));
    }
    return NONE_OP;
}

static predicate parse_predicate(const char *data, int len, Oid item_type)
{
    predicate pred;
    bool any_val = 0;
    int pos = 0;

    pred.obj_type = item_type;

    if (len == 0)
    {
        pred.op = NONE_OP;
        return pred;
    }

    while (pos < len && data[pos] >= 'a' && data[pos] <= 'z')
    {
        any_val = 1;
        pos++;
    }

    if (!any_val)
        ereport(ERROR, errmsg("unsupported predicate"));

    while (pos < len && data[pos] == ' ')
        pos++;
    if (pos >= len)
        ereport(ERROR, errmsg("unsupported predicate"));

    pred.op = parse_operation(data, len, &pos);

    if ((item_type == TEXTOID || item_type == VARCHAROID || item_type == BPCHAROID) &&
        !(pred.op == EQ_OP || pred.op == N_EQ_OP))
        ereport(ERROR, errmsg("unsupported predicate"));
    
    if (pred.op == MOD_OP &&
        !(item_type == INT2OID || item_type == INT4OID || item_type == INT8OID))
        ereport(ERROR, errmsg("unsupported predicate"));


    while (pos < len && data[pos] == ' ')
        pos++;
    if (pos >= len)
        ereport(ERROR, errmsg("unsupported predicate"));

    pred.obj1 = parse_item(data, len, &pos, item_type);
    
    while (pos < len && data[pos] == ' ')
        pos++;

    if (pos >= len)
        if (pred.op != MOD_OP)
            return pred;
        else
            ereport(ERROR, errmsg("unsupported predicate"));
    
    if (pred.op != MOD_OP || data[pos] != '=')
        ereport(ERROR, errmsg("unsupported predicate"));

    pos++;

    while (pos < len && data[pos] == ' ')
        pos++;
    if (pos >= len)
        ereport(ERROR, errmsg("unsupported predicate"));

    pred.obj2 = parse_item(data, len, &pos, item_type);

    while (pos < len && data[pos] == ' ')
        pos++;
    if (pos < len)
        ereport(ERROR, errmsg("unsupported predicate"));

    return pred;
}

static bool process_predicate(Datum obj, Oid obj_type, predicate pred, Oid collation)
{
    if (pred.obj_type != obj_type)
        ereport(ERROR, errmsg("predicate type does not match element type"));

    switch (pred.op)
    {
    case EQ_OP:
        return compare(obj_type, obj, pred.obj1, collation) == 0;
    case N_EQ_OP:
        return compare(obj_type, obj, pred.obj1, collation) != 0;
    case GR_OP:
        return compare(obj_type, obj, pred.obj1, collation) > 0;
    case EQ_GR_OP:
        return compare(obj_type, obj, pred.obj1, collation) >= 0;
    case LE_OP:
        return compare(obj_type, obj, pred.obj1, collation) < 0;
    case EQ_LE_OP:
        return compare(obj_type, obj, pred.obj1, collation) <= 0;
    case NONE_OP:
        return 1;
    case MOD_OP:
        switch (obj_type)
        {
        case INT2OID:
            if (DatumGetInt16(pred.obj1) == 0)
                ereport(ERROR, errmsg("division by zero"));
            return compare(obj_type, Int16GetDatum(DatumGetInt16(obj) % DatumGetInt16(pred.obj1)), pred.obj2, collation) == 0;
        case INT4OID:
            if (DatumGetInt32(pred.obj1) == 0)
                ereport(ERROR, errmsg("division by zero"));
            return compare(obj_type, Int32GetDatum(DatumGetInt32(obj) % DatumGetInt32(pred.obj1)), pred.obj2, collation) == 0;
        case INT8OID:
            if (DatumGetInt64(pred.obj1) == 0)
                ereport(ERROR, errmsg("division by zero"));
            return compare(obj_type, Int64GetDatum(DatumGetInt64(obj) % DatumGetInt64(pred.obj1)), pred.obj2, collation) == 0;
        default:
            ereport(ERROR, errmsg("unsupported operation"));
        }
        break;
    default:
        ereport(ERROR, errmsg("unsupported operation"));
    }
    return 0;
}

Datum
array_contains(PG_FUNCTION_ARGS)
{
    ArrayType      *array;
    Datum           searched_elem;
    Oid             elem_type;
	Oid             collation;
    Datum          *vals;
    bool           *nulls;
	int16           typlen;
	bool            typbyval;
	char            typalign;
    int             n, i;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    array = PG_GETARG_ARRAYTYPE_P(0);
	elem_type = ARR_ELEMTYPE(array);
	
	get_typlenbyvalalign(elem_type, &typlen, &typbyval, &typalign);
    deconstruct_array(array, elem_type, typlen, typbyval, typalign, &vals, &nulls, &n);

    if (PG_ARGISNULL(1))
	{
	    for (i = 0; i < n; i++)
			if (nulls[i])
            {
                pfree(vals);
                pfree(nulls);
            	PG_RETURN_BOOL(true);
            }
        pfree(vals);
        pfree(nulls);
	    PG_RETURN_BOOL(false);
    }

	searched_elem  = PG_GETARG_DATUM(1);
    
	collation = PG_GET_COLLATION();

    for (i = 0; i < n; i++)
    {
        if (nulls[i])
            continue;
        if (!compare(elem_type, vals[i], searched_elem, collation))
        {
            pfree(vals);
            pfree(nulls);
            PG_RETURN_BOOL(true);
        }
    }

    pfree(vals);
    pfree(nulls);
    PG_RETURN_BOOL(false);
}

Datum
array_contains_all(PG_FUNCTION_ARGS)
{
    ArrayType      *array;
    ArrayType      *searched_array;
    Oid             elem_type;
    Oid             collation;
    Datum          *vals;
    Datum          *searched_vals;
    bool           *nulls;
    bool           *searched_nulls;
    int16           typlen;
    bool            typbyval;
    char            typalign;
    int             n, i;
    int             searched_n, j;
    bool            found;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        PG_RETURN_NULL();

    array = PG_GETARG_ARRAYTYPE_P(0);
    searched_array = PG_GETARG_ARRAYTYPE_P(1);

    elem_type = ARR_ELEMTYPE(searched_array);

    get_typlenbyvalalign(elem_type, &typlen, &typbyval, &typalign);
    deconstruct_array(array, elem_type, typlen, typbyval, typalign, &vals, &nulls, &n);
    deconstruct_array(searched_array, elem_type, typlen, typbyval, typalign, &searched_vals, &searched_nulls, &searched_n);

    collation = PG_GET_COLLATION();

    for (j = 0; j < searched_n; j++)
    {
        found = 0;

        if (searched_nulls[j])
        {
            for (i = 0; i < n; i++)
            {
                if (nulls[i])
                {
                    found = 1;
                    break;
                }
            }
            if (!found)
            {
                pfree(searched_vals);
                pfree(searched_nulls);
                pfree(vals);
                pfree(nulls);
                PG_RETURN_BOOL(false);
            }
            continue;
        }

        for (i = 0; i < n; i++)
        {
            if (nulls[i]) continue;

            if (!compare(elem_type, vals[i], searched_vals[j], collation))
            {
                found = 1;
                break;
            }
        }

        if (!found)
        {
            pfree(searched_vals);
            pfree(searched_nulls);
            pfree(vals);
            pfree(nulls);
            PG_RETURN_BOOL(false);
        }
    }

    pfree(searched_vals);
    pfree(searched_nulls);
    pfree(vals);
    pfree(nulls);
    PG_RETURN_BOOL(true);
}

Datum
array_find_all(PG_FUNCTION_ARGS)
{
    ArrayType      *array;
    ArrayType      *positions_array;
    Datum           searched_elem;
    Oid             elem_type;
	Oid             collation;
    Datum          *vals;
    bool           *nulls;
	int16           typlen;
	bool            typbyval;
	char            typalign;
    int             n;
    int32           i;
    int32           pos;
    Datum          *positions;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    array = PG_GETARG_ARRAYTYPE_P(0);

    if (ARR_NDIM(array) > 1)
        ereport(ERROR, errmsg("multidimensional arrays is not supported"));

	elem_type = ARR_ELEMTYPE(array);
	
	get_typlenbyvalalign(elem_type, &typlen, &typbyval, &typalign);
    deconstruct_array(array, elem_type, typlen, typbyval, typalign, &vals, &nulls, &n);

    if (n == 0)
    {
        PG_RETURN_ARRAYTYPE_P(construct_empty_array(INT4OID));
    }

    positions = palloc(n*sizeof(Datum));
    pos = 0;

    if (PG_ARGISNULL(1))
	{
	    for (i = 0; i < n; i++)
			if (nulls[i])
            {
                positions[pos++] = Int32GetDatum(i + 1);
            }

            positions_array = construct_array(
                positions, 
                pos, 
                INT4OID, 
                sizeof(int32), 
                true,
                'i'
            );

            pfree(vals);
            pfree(nulls);
            pfree(positions);

            PG_RETURN_ARRAYTYPE_P(positions_array);
    }

	searched_elem  = PG_GETARG_DATUM(1);
    
	collation = PG_GET_COLLATION();

    for (i = 0; i < n; i++)
    {
        if (nulls[i])
            continue;
        if (!compare(elem_type, vals[i], searched_elem, collation))
        {
            positions[pos++] = Int32GetDatum(i + 1);
        }
    }

    positions_array = construct_array(
                positions, 
                pos, 
                INT4OID, 
                sizeof(int32), 
                true,
                'i'
            );

    pfree(vals);
    pfree(nulls);
    pfree(positions);

    PG_RETURN_ARRAYTYPE_P(positions_array);
}

Datum
array_filter(PG_FUNCTION_ARGS)
{
    ArrayType      *array;
    ArrayType      *result_array;
    Oid             elem_type;
	Oid             collation;
    Datum          *vals;
    bool           *nulls;
	int16           typlen;
	bool            typbyval;
	char            typalign;
    int             n;
    int32           i;
    int32           pos;
    Datum          *result;
    size_t          pred_buf_len;
    char           *pred_buf;
    text           *pred_text;
    predicate       pred;


    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        PG_RETURN_NULL();

    array = PG_GETARG_ARRAYTYPE_P(0);
	elem_type = ARR_ELEMTYPE(array);
	
	get_typlenbyvalalign(elem_type, &typlen, &typbyval, &typalign);
    deconstruct_array(array, elem_type, typlen, typbyval, typalign, &vals, &nulls, &n);

    if (n == 0)
    {
        PG_RETURN_ARRAYTYPE_P(construct_empty_array(elem_type));
    }
    
    result = palloc(n*sizeof(Datum));
    pos = 0;

	collation = PG_GET_COLLATION();

    pred_text = PG_GETARG_TEXT_PP(1);

    pred_buf_len = VARSIZE_ANY_EXHDR(pred_text);
    pred_buf = VARDATA_ANY(pred_text);

    pred = parse_predicate(pred_buf, pred_buf_len, elem_type);

    for (i = 0; i < n; i++)
    {
        if (nulls[i])
            continue;
        if (process_predicate(vals[i], elem_type, pred, collation))
        {
            result[pos++] = vals[i];
        }
    }

    result_array = construct_array(
                result, 
                pos,
                elem_type,
                typlen, 
                typbyval, 
                typalign
            );

    pfree(vals);
    pfree(nulls);
    pfree(result);

    PG_RETURN_ARRAYTYPE_P(result_array);
}

Datum
array_reject(PG_FUNCTION_ARGS)
{
    ArrayType      *array;
    ArrayType      *result_array;
    Oid             elem_type;
	Oid             collation;
    Datum          *vals;
    bool           *nulls;
	int16           typlen;
	bool            typbyval;
	char            typalign;
    int             n;
    int32           i;
    int32           pos;
    Datum          *result;
    size_t          pred_buf_len;
    char           *pred_buf;
    text           *pred_text;
    predicate       pred;


    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        PG_RETURN_NULL();

    array = PG_GETARG_ARRAYTYPE_P(0);
	elem_type = ARR_ELEMTYPE(array);
	
	get_typlenbyvalalign(elem_type, &typlen, &typbyval, &typalign);
    deconstruct_array(array, elem_type, typlen, typbyval, typalign, &vals, &nulls, &n);

    if (n == 0)
    {
        PG_RETURN_ARRAYTYPE_P(construct_empty_array(elem_type));
    }
    
    result = palloc(n*sizeof(Datum));
    pos = 0;

	collation = PG_GET_COLLATION();

    pred_text = PG_GETARG_TEXT_PP(1);

    pred_buf_len = VARSIZE_ANY_EXHDR(pred_text);
    pred_buf = VARDATA_ANY(pred_text);

    pred = parse_predicate(pred_buf, pred_buf_len, elem_type);

    for (i = 0; i < n; i++)
    {
        if (nulls[i])
            continue;
        if (!process_predicate(vals[i], elem_type, pred, collation))
        {
            result[pos++] = vals[i];
        }
    }

    result_array = construct_array(
                result, 
                pos,
                elem_type,
                typlen, 
                typbyval, 
                typalign
            );

    pfree(vals);
    pfree(nulls);
    pfree(result);

    PG_RETURN_ARRAYTYPE_P(result_array);
}

Datum
array_partition(PG_FUNCTION_ARGS)
{
    ArrayType      *array;
    ArrayType      *result_array;
    Oid             elem_type;
	Oid             collation;
    Datum          *vals;
    bool           *nulls;
	int16           typlen;
	bool            typbyval;
	char            typalign;
    int             n;
    int32           i;
    int32           pos_ok;
    int32           pos_no;
    Datum          *result_ok;
    Datum          *result_no;
    bool           *result_nulls;
    size_t          pred_buf_len;
    char           *pred_buf;
    text           *pred_text;
    predicate       pred;
    int             ndims = 2;
    int             dims[2] = {2, 0};
    int             lbs[2] = {1, 1};

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        PG_RETURN_NULL();

    array = PG_GETARG_ARRAYTYPE_P(0);
	elem_type = ARR_ELEMTYPE(array);
	
	get_typlenbyvalalign(elem_type, &typlen, &typbyval, &typalign);
    deconstruct_array(array, elem_type, typlen, typbyval, typalign, &vals, &nulls, &n);

    if (n == 0)
    {
        PG_RETURN_ARRAYTYPE_P(construct_empty_array(elem_type));
    }
    
    result_ok = palloc(n*sizeof(Datum)*2);
    pos_ok = 0;
    result_no = palloc(n*sizeof(Datum));
    pos_no = 0;

	collation = PG_GET_COLLATION();

    pred_text = PG_GETARG_TEXT_PP(1);

    pred_buf_len = VARSIZE_ANY_EXHDR(pred_text);
    pred_buf = VARDATA_ANY(pred_text);

    pred = parse_predicate(pred_buf, pred_buf_len, elem_type);

    for (i = 0; i < n; i++)
    {
        if (nulls[i])
            continue;
        if (process_predicate(vals[i], elem_type, pred, collation))
        {
            result_ok[pos_ok++] = vals[i];
        }
        else
        {
            result_no[pos_no++] = vals[i];
        }
    }

    result_nulls = palloc0(n*sizeof(bool)*2);

    if (pos_ok == pos_no)
    {
        memcpy(result_ok + pos_ok, result_no, pos_no*sizeof(Datum));
        dims[1] = pos_ok;
    }
    else if (pos_ok > pos_no)
    {
        memcpy(result_ok + pos_ok, result_no, pos_no*sizeof(Datum));
        memset(result_nulls + pos_ok + pos_no, true, (pos_ok - pos_no) * sizeof(bool));
        dims[1] = pos_ok;
    }
    else
    {
        memcpy(result_ok + pos_no, result_no, pos_no*sizeof(Datum));
        memset(result_nulls + pos_ok, true, (pos_no - pos_ok) * sizeof(bool));
        dims[1] = pos_no;
    }

    result_array = construct_md_array(
                result_ok, 
                result_nulls,
                ndims,
                dims,
                lbs,
                elem_type,
                typlen, 
                typbyval, 
                typalign
            );

    pfree(vals);
    pfree(nulls);
    pfree(result_ok);
    pfree(result_no);
    pfree(result_nulls);

    PG_RETURN_ARRAYTYPE_P(result_array);
}

Datum
array_index_of(PG_FUNCTION_ARGS)
{
    ArrayType      *array;
    Datum           searched_elem;
    Oid             elem_type;
	Oid             collation;
    Datum          *vals;
    bool           *nulls;
	int16           typlen;
	bool            typbyval;
	char            typalign;
    int             n;
    int32           i;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    array = PG_GETARG_ARRAYTYPE_P(0);

    if (ARR_NDIM(array) > 1)
        ereport(ERROR, errmsg("multidimensional arrays is not supported"));

	elem_type = ARR_ELEMTYPE(array);
	
	get_typlenbyvalalign(elem_type, &typlen, &typbyval, &typalign);
    deconstruct_array(array, elem_type, typlen, typbyval, typalign, &vals, &nulls, &n);

    if (PG_ARGISNULL(1))
	{
	    for (i = 0; i < n; i++)
			if (nulls[i])
            {
                pfree(vals);
                pfree(nulls);
            	PG_RETURN_INT32(i + 1);
            }
        pfree(vals);
        pfree(nulls);
	    PG_RETURN_INT32(-1);
    }

	searched_elem  = PG_GETARG_DATUM(1);
    
	collation = PG_GET_COLLATION();

    for (i = 0; i < n; i++)
    {
        if (nulls[i])
            continue;
        if (!compare(elem_type, vals[i], searched_elem, collation))
        {
            pfree(vals);
            pfree(nulls);
            PG_RETURN_INT32(i + 1);
        }
    }

    pfree(vals);
    pfree(nulls);
    PG_RETURN_INT32(-1);
}

Datum
array_last_index_of(PG_FUNCTION_ARGS)
{
    ArrayType      *array;
    Datum           searched_elem;
    Oid             elem_type;
	Oid             collation;
    Datum          *vals;
    bool           *nulls;
	int16           typlen;
	bool            typbyval;
	char            typalign;
    int             n;
    int32           i;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    array = PG_GETARG_ARRAYTYPE_P(0);

    if (ARR_NDIM(array) > 1)
        ereport(ERROR, errmsg("multidimensional arrays is not supported"));

	elem_type = ARR_ELEMTYPE(array);
	
	get_typlenbyvalalign(elem_type, &typlen, &typbyval, &typalign);
    deconstruct_array(array, elem_type, typlen, typbyval, typalign, &vals, &nulls, &n);

    if (PG_ARGISNULL(1))
	{
	    for (i = n - 1; i >= 0; i--)
			if (nulls[i])
            {
                pfree(vals);
                pfree(nulls);
            	PG_RETURN_INT32(i + 1);
            }
        pfree(vals);
        pfree(nulls);
	    PG_RETURN_INT32(-1);
    }

	searched_elem  = PG_GETARG_DATUM(1);
    
	collation = PG_GET_COLLATION();

    for (i = n - 1; i >= 0; i--)
    {
        if (nulls[i])
            continue;
        if (!compare(elem_type, vals[i], searched_elem, collation))
        {
            pfree(vals);
            pfree(nulls);
            PG_RETURN_INT32(i + 1);
        }
    }

    pfree(vals);
    pfree(nulls);
    PG_RETURN_INT32(-1);
}