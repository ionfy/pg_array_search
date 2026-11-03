#include "postgres.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "catalog/pg_type.h"
#include "utils/memutils.h"

#include <string.h>

static const Size PG_ARRAY_SEARCH_MAX_ALLOWED_SIZE = (MaxAllocSize / sizeof(Datum));

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

static inline bool is_int_type_supported(Oid type)
{
    return type == INT2OID || type == INT4OID || type == INT8OID;
}

static inline bool is_numeric_type_supported(Oid type)
{
    return is_int_type_supported(type) ||
            type == FLOAT4OID || type == FLOAT8OID || type == NUMERICOID;
}

static inline bool is_string_type_supported(Oid type)
{
    return type == TEXTOID || type == VARCHAROID || type == BPCHAROID;
}

static inline bool is_type_supported(Oid type)
{
    return is_numeric_type_supported(type) || is_string_type_supported(type);
}

static void check_supported_type(Oid type)
{
    if (is_type_supported(type))
    {
        return;
    }
    
    ereport(ERROR, (errmsg("unsupported data type"),
                    errdetail("Data type %s is not supported.",
                        format_type_be(type)),
                    errhint("Supported data types: smallint, integer, bigint, "
                        "real, double precision, numeric, text, character varying, character.")));
}

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
        ereport(ERROR, (errmsg("unsupported data type for comparison"),
                        errdetail("Data type %s is not supported.",
                            format_type_be(type))));
    }

    return cmp;
}

static Datum parse_item(const char *data, int len, int* pos, Oid item_type)
{
    int item_len = 0;
    Datum item;
    bool is_float = 0;
    char* buf;

    if ((*pos) >= len)
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                            errdetail("Expected a constant value, but reached the end of the expression.")));
    }

    if (is_string_type_supported(item_type))
    {
        if (data[(*pos)] != '\"')
        {
            ereport(ERROR, (errmsg("incorrect predicate syntax"),
                            errdetail("Double quotes were expected.")));
        }
        
        (*pos)++;
        while ((*pos) + item_len < len && data[(*pos) + item_len] != '"')
	    {
		    item_len++;
	    }
        if ((*pos) + item_len >= len)
        {
            ereport(ERROR, (errmsg("incorrect predicate syntax"),
                            errdetail("Expected the end of the string (\"), but reached the end of the expression.")));
        }

        buf = (char*)palloc(sizeof(char)*(item_len+1));
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
            pfree(buf);
            ereport(ERROR, (errmsg("incorrect predicate syntax"),
                            errdetail("Array element type %s is not a supported string type.",
                                format_type_be(item_type))));
        }

        (*pos) += item_len + 1;

        pfree(buf);
    }
    else if (is_numeric_type_supported(item_type))
    {
        
        if ((*pos) + item_len < len && data[(*pos) + item_len] == '-')
        {
            item_len++;
        }
        
        while ((*pos) + item_len < len &&
            ((data[(*pos) + item_len] >= '0' && data[(*pos) + item_len] <= '9') ||
            data[(*pos) + item_len] == '.'))
        {
            if (data[(*pos) + item_len] == '.')
            {
                if (is_float)
                {
                    ereport(ERROR, (errmsg("incorrect predicate syntax"),
                                    errdetail("A numeric constant cannot contain more than one decimal point.")));
                }
                is_float = 1;
            }
            item_len++;
        }

        if (item_len == 0)
        {
            ereport(ERROR, (errmsg("incorrect predicate syntax"),
                            errdetail("Expected a numeric value.")));
        }

        if (item_len == 1 && data[(*pos)] == '-')
        {
            ereport(ERROR, (errmsg("incorrect predicate syntax"),
                            errdetail("Expected a numeric value.")));
        }

        if (is_float && (item_type == INT2OID || item_type == INT4OID || item_type == INT8OID))
        {
            ereport(ERROR, (errmsg("incorrect predicate syntax"),
                            errdetail("Cannot compare an integer array with a floating-point value.")));
        }

        buf = (char*)palloc(sizeof(char)*(item_len+1));
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
                pfree(buf);
                ereport(ERROR, (errmsg("incorrect predicate syntax"),
                            errdetail("Array element type %s is not a supported numeric type.",
                                format_type_be(item_type))));
            }

        (*pos) += item_len;

        pfree(buf);
    }
    else
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                            errdetail("Array element type %s is not supported.",
                                format_type_be(item_type))));
    }

    return item;
}

static predicate_op parse_operation(const char *data, int len, int* pos)
{
    if ((*pos) >= len)
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                            errdetail("Expected an operation, but reached the end of the expression.")));
    }
    switch (data[(*pos)])
    {
    case '=':
        if ((*pos) + 1 < len && data[(*pos) + 1] == '=')
        {
            (*pos)++;
        }
        (*pos)++;
        return EQ_OP;
    case '!':
        if (((*pos) + 1 >= len) || (data[(*pos) + 1] != '='))
        {
            ereport(ERROR, (errmsg("incorrect predicate syntax"),
                            errdetail("Unsupported operation.")));
        }
        (*pos)+=2;
        return N_EQ_OP;
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
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                            errdetail("Unsupported operation.")));
    }
    ereport(ERROR, (errmsg("incorrect predicate syntax"),
                            errdetail("Unsupported operation.")));
}

static predicate parse_predicate(const char *data, int len, Oid item_type)
{
    predicate pred;
    bool any_val = 0;
    int pos = 0;

    pred.obj_type = item_type;

    while (pos < len && data[pos] == ' ')
    {
        pos++;
    }
    if (pos >= len)
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
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Expected a variable name.")));
    }

    while (pos < len && data[pos] == ' ')
    {
        pos++;
    }
    if (pos >= len)
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Expected an operation, but reached the end of the expression.")));
    }

    pred.op = parse_operation(data, len, &pos);

    if (is_string_type_supported(item_type) &&
        !(pred.op == EQ_OP || pred.op == N_EQ_OP))
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("String types support equality operators only.")));
    }
    
    if (pred.op == MOD_OP &&
        !is_int_type_supported(item_type))
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Operator '%%' can only be used with integer array types.")));
    }


    while (pos < len && data[pos] == ' ')
    {
        pos++;
    }
    if (pos >= len)
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Expected a constant value, but reached the end of the expression.")));
    }

    pred.obj1 = parse_item(data, len, &pos, item_type);
    
    while (pos < len && data[pos] == ' ')
    {
        pos++;
    }

    if (pos >= len)
    {
        if (pred.op != MOD_OP)
        {
            return pred;
        }
        else
        {
            ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Expected '=' operator.")));
        }
    }
    
    if (pred.op != MOD_OP)
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Expected the end of the expression.")));
    }
    if (data[pos] != '=')
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Expected '=' operator.")));
    }

    pos++;

    while (pos < len && data[pos] == ' ')
    {
        pos++;
    }
    if (pos >= len)
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Expected a constant value after '=', but reached the end of the expression.")));
    }

    pred.obj2 = parse_item(data, len, &pos, item_type);

    while (pos < len && data[pos] == ' ')
    {
        pos++;
    }
    if (pos < len)
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Expected the end of the expression.")));
    }

    return pred;
}

static bool process_predicate(Datum obj, Oid obj_type, predicate pred, Oid collation)
{
    Datum mod;

    Assert(pred.obj_type == obj_type);

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
            {
                ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Division by zero is not allowed.")));
            }
            mod = DirectFunctionCall2(int2mod, obj, pred.obj1);
            return compare(obj_type, mod, pred.obj2, collation) == 0;
        case INT4OID:
            if (DatumGetInt32(pred.obj1) == 0)
            {
                ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Division by zero is not allowed.")));
            }
            mod = DirectFunctionCall2(int4mod, obj, pred.obj1);
            return compare(obj_type, mod, pred.obj2, collation) == 0;
        case INT8OID:
            if (DatumGetInt64(pred.obj1) == 0)
            {
                ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Division by zero is not allowed.")));
            }
            mod = DirectFunctionCall2(int8mod, obj, pred.obj1);
            return compare(obj_type, mod, pred.obj2, collation) == 0;
        default:
            ereport(ERROR, errmsg("unsupported operator for executing the predicate"));
        }
        break;
    default:
        ereport(ERROR, errmsg("unsupported operator for executing the predicate"));
    }
    ereport(ERROR, errmsg("unsupported operator for executing the predicate"));
}

Datum
array_contains(PG_FUNCTION_ARGS)
{
    ArrayType      *array;
    Datum           searched_elem;
    Oid             elem_type;
	Oid             collation;
    ArrayIterator   iterator;
    Datum           element;
    bool            isnull;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    array = PG_GETARG_ARRAYTYPE_P(0);
	elem_type = ARR_ELEMTYPE(array);

    check_supported_type(elem_type);

    iterator = array_create_iterator(array, 0, NULL);

    if (PG_ARGISNULL(1))
	{
        while (array_iterate(iterator, &element, &isnull))
        {
            if (isnull)
            {
                array_free_iterator(iterator);
                PG_RETURN_BOOL(true);
            }
        }

        array_free_iterator(iterator);
        PG_RETURN_BOOL(false);
    }

	searched_elem  = PG_GETARG_DATUM(1);
	collation = PG_GET_COLLATION();

    while (array_iterate(iterator, &element, &isnull))
    {
        if (isnull)
        {
            continue;
        }

        if (!compare(elem_type, element, searched_elem, collation))
        {
            array_free_iterator(iterator);
            PG_RETURN_BOOL(true);
        }
    }

    array_free_iterator(iterator);
    PG_RETURN_BOOL(false);
}

Datum
array_contains_all(PG_FUNCTION_ARGS)
{
    ArrayType      *array;
    ArrayType      *searched_array;
    Oid             elem_type;
    Oid             collation;
    ArrayIterator   iterator;
    Datum           element;
    bool            isnull;
    ArrayIterator   searched_iterator;
    Datum           searched_element;
    bool            searched_isnull;
    bool            found;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    array = PG_GETARG_ARRAYTYPE_P(0);
    searched_array = PG_GETARG_ARRAYTYPE_P(1);

    elem_type = ARR_ELEMTYPE(searched_array);

    check_supported_type(elem_type);

    Assert(elem_type == ARR_ELEMTYPE(array));

    collation = PG_GET_COLLATION();

    searched_iterator = array_create_iterator(searched_array, 0, NULL);

    while (array_iterate(searched_iterator, &searched_element, &searched_isnull))
    {
        found = false;

        iterator = array_create_iterator(array, 0, NULL);

        if (searched_isnull)
        {
            while (array_iterate(iterator, &element, &isnull))
            {
                if (isnull)
                {
                    found = true;
                    break;
                }
            }
        }
        else
        {
            while (array_iterate(iterator, &element, &isnull))
            {
                if (isnull)
                {
                    continue;
                }
                if (!compare(elem_type, element, searched_element, collation))
                {
                    found = true;
                    break;
                }
            }
        }

        array_free_iterator(iterator);

        if (!found)
        {
            array_free_iterator(searched_iterator);
            PG_RETURN_BOOL(false);
        }
    }

    array_free_iterator(searched_iterator);
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
	int16           typlen;
	bool            typbyval;
	char            typalign;
    ArrayIterator   iterator;
    Datum           element;
    bool            isnull;
    int32           size;
    int32           pos;
    int32           positions_size;
    Datum          *positions;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    array = PG_GETARG_ARRAYTYPE_P(0);

    if (ARR_NDIM(array) > 1)
    {
        ereport(ERROR, errmsg("multidimensional arrays is not supported"));
    }

    size = ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array));

	elem_type = ARR_ELEMTYPE(array);

    check_supported_type(elem_type);

    if (size <= 0)
    {
        PG_RETURN_ARRAYTYPE_P(construct_empty_array(INT4OID));
    }

    if (size > PG_ARRAY_SEARCH_MAX_ALLOWED_SIZE)
    {
        ereport(ERROR, errmsg("array size exceeds the maximum allowed (%zu)", PG_ARRAY_SEARCH_MAX_ALLOWED_SIZE));
    }

    positions = (Datum*)palloc(sizeof(Datum)*size);
    positions_size = 0;

    iterator = array_create_iterator(array, 0, NULL);

    pos = 1;

    if (PG_ARGISNULL(1))
	{
        while (array_iterate(iterator, &element, &isnull))
        {
            if (isnull)
            {
                positions[positions_size++] = Int32GetDatum(pos);
            }

            pos++;
        }
    }
    else
    {
        searched_elem  = PG_GETARG_DATUM(1);
	    collation = PG_GET_COLLATION();

        while (array_iterate(iterator, &element, &isnull))
        {
            if (!isnull && !compare(elem_type, element, searched_elem, collation))
            {
                positions[positions_size++] = Int32GetDatum(pos);
            }

            pos++;
        }
    }

    array_free_iterator(iterator);

    get_typlenbyvalalign(INT4OID, &typlen, &typbyval, &typalign);

    positions_array = construct_array(
                positions, 
                positions_size, 
                INT4OID, 
                typlen, 
                typbyval,
                typalign
            );

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
    ArrayIterator   iterator;
    Datum           element;
    bool            isnull;
	int16           typlen;
	bool            typbyval;
	char            typalign;
    int32           size;
    int32           result_size;
    Datum          *result;
    size_t          pred_buf_len;
    char           *pred_buf;
    text           *pred_text;
    predicate       pred;


    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    array = PG_GETARG_ARRAYTYPE_P(0);
	elem_type = ARR_ELEMTYPE(array);

    check_supported_type(elem_type);

    if (ARR_NDIM(array) > 1)
    {
        ereport(ERROR, errmsg("multidimensional arrays is not supported"));
    }
	
    size = ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array));

    if (size <= 0)
    {
        PG_RETURN_ARRAYTYPE_P(construct_empty_array(elem_type));
    }

    if (size > PG_ARRAY_SEARCH_MAX_ALLOWED_SIZE)
    {
        ereport(ERROR, errmsg("array size exceeds the maximum allowed (%zu)", PG_ARRAY_SEARCH_MAX_ALLOWED_SIZE));
    }
    
    result = (Datum*)palloc(sizeof(Datum)*size);
    result_size = 0;

	collation = PG_GET_COLLATION();

    pred_text = PG_GETARG_TEXT_PP(1);
    pred_buf_len = VARSIZE_ANY_EXHDR(pred_text);
    pred_buf = VARDATA_ANY(pred_text);
    pred = parse_predicate(pred_buf, pred_buf_len, elem_type);

    iterator = array_create_iterator(array, 0, NULL);

    while (array_iterate(iterator, &element, &isnull))
    {
        if (isnull)
        {
            continue;
        }

        if (process_predicate(element, elem_type, pred, collation))
        {
            result[result_size++] = element;
        }
    }

    array_free_iterator(iterator);

    get_typlenbyvalalign(elem_type, &typlen, &typbyval, &typalign);
    result_array = construct_array(
                result, 
                result_size,
                elem_type,
                typlen, 
                typbyval, 
                typalign
            );

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
    ArrayIterator   iterator;
    Datum           element;
    bool            isnull;
	int16           typlen;
	bool            typbyval;
	char            typalign;
    int32           size;
    int32           result_size;
    Datum          *result;
    size_t          pred_buf_len;
    char           *pred_buf;
    text           *pred_text;
    predicate       pred;


    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    array = PG_GETARG_ARRAYTYPE_P(0);
	elem_type = ARR_ELEMTYPE(array);

    check_supported_type(elem_type);

    if (ARR_NDIM(array) > 1)
    {
        ereport(ERROR, errmsg("multidimensional arrays is not supported"));
    }
	
    size = ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array));

    if (size <= 0)
    {
        PG_RETURN_ARRAYTYPE_P(construct_empty_array(elem_type));
    }

    if (size > PG_ARRAY_SEARCH_MAX_ALLOWED_SIZE)
    {
        ereport(ERROR, errmsg("array size exceeds the maximum allowed (%zu)", PG_ARRAY_SEARCH_MAX_ALLOWED_SIZE));
    }
    
    result = (Datum*)palloc(sizeof(Datum)*size);
    result_size = 0;

	collation = PG_GET_COLLATION();

    pred_text = PG_GETARG_TEXT_PP(1);
    pred_buf_len = VARSIZE_ANY_EXHDR(pred_text);
    pred_buf = VARDATA_ANY(pred_text);
    pred = parse_predicate(pred_buf, pred_buf_len, elem_type);

    iterator = array_create_iterator(array, 0, NULL);

    while (array_iterate(iterator, &element, &isnull))
    {
        if (isnull)
        {
            continue;
        }

        if (!process_predicate(element, elem_type, pred, collation))
        {
            result[result_size++] = element;
        }
    }

    array_free_iterator(iterator);

    get_typlenbyvalalign(elem_type, &typlen, &typbyval, &typalign);
    result_array = construct_array(
                result, 
                result_size,
                elem_type,
                typlen, 
                typbyval, 
                typalign
            );

    pfree(result);
    PG_RETURN_ARRAYTYPE_P(result_array);
}

Datum
array_partition(PG_FUNCTION_ARGS)
{
    ArrayType      *array;
    ArrayType      *result_array;
    ArrayIterator   iterator;
    Datum           element;
    bool            isnull;
    Oid             elem_type;
	Oid             collation;
	int16           typlen;
	bool            typbyval;
	char            typalign;
    int32           size_ok;
    int32           size_no;
    int32           size_max;
    int32           size;
    Datum          *result;
    bool           *result_nulls;
    size_t          pred_buf_len;
    char           *pred_buf;
    text           *pred_text;
    predicate       pred;
    int             ndims = 2;
    int             dims[2] = {2, 0};
    int             lbs[2] = {1, 1};

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    array = PG_GETARG_ARRAYTYPE_P(0);
	elem_type = ARR_ELEMTYPE(array);

    check_supported_type(elem_type);

    if (ARR_NDIM(array) > 1)
    {
        ereport(ERROR, errmsg("multidimensional arrays is not supported"));
    }
	
	get_typlenbyvalalign(elem_type, &typlen, &typbyval, &typalign);

    size = ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array));

    if (size <= 0)
    {
        PG_RETURN_ARRAYTYPE_P(construct_empty_array(elem_type));
    }

    if (size > PG_ARRAY_SEARCH_MAX_ALLOWED_SIZE / 2)
    {
        ereport(ERROR, errmsg("array size exceeds the maximum allowed (%zu)", PG_ARRAY_SEARCH_MAX_ALLOWED_SIZE / 2));
    }
    
    result = (Datum*)palloc(sizeof(Datum)*size*2);
    size_ok = 0;
    size_no = 0;

	collation = PG_GET_COLLATION();

    pred_text = PG_GETARG_TEXT_PP(1);

    pred_buf_len = VARSIZE_ANY_EXHDR(pred_text);
    pred_buf = VARDATA_ANY(pred_text);

    pred = parse_predicate(pred_buf, pred_buf_len, elem_type);

    iterator = array_create_iterator(array, 0, NULL);

    while (array_iterate(iterator, &element, &isnull))
    {
        if (isnull)
        {
            continue;
        }

        if (process_predicate(element, elem_type, pred, collation))
        {
            result[size_ok++] = element;
        }
        else
        {
            result[size + (size_no++)] = element;
        }
    }

    array_free_iterator(iterator);

    size_max = (size_ok > size_no) ? size_ok : size_no;

    result_nulls = (bool*)palloc0(sizeof(bool)*size_max*2);

    dims[1] = size_max;

    memmove(result + size_max, result + size, size_no*sizeof(Datum));

    if (size_ok < size_max)
    {
        memset(result_nulls + size_ok, true, (size_max - size_ok) * sizeof(bool));
    }
    if (size_no < size_max)
    {
        memset(result_nulls + size_max + size_no, true, (size_max - size_no) * sizeof(bool));
    }

    result_array = construct_md_array(
                result, 
                result_nulls,
                ndims,
                dims,
                lbs,
                elem_type,
                typlen, 
                typbyval, 
                typalign
            );

    pfree(result);
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
    ArrayIterator   iterator;
    Datum           element;
    bool            isnull;
    int32           pos;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    array = PG_GETARG_ARRAYTYPE_P(0);

    if (ARR_NDIM(array) > 1)
    {
        ereport(ERROR, errmsg("multidimensional arrays is not supported"));
    }

	elem_type = ARR_ELEMTYPE(array);

    check_supported_type(elem_type);

    pos = 1;

    iterator = array_create_iterator(array, 0, NULL);

    if (PG_ARGISNULL(1))
	{
        while (array_iterate(iterator, &element, &isnull))
        {
            if (isnull)
            {
                array_free_iterator(iterator);
                PG_RETURN_INT32(pos);
            }

            pos++;
        }

        array_free_iterator(iterator);
        PG_RETURN_INT32(-1);
    }

	searched_elem  = PG_GETARG_DATUM(1);
	collation = PG_GET_COLLATION();

    while (array_iterate(iterator, &element, &isnull))
    {
        if (!isnull && !compare(elem_type, element, searched_elem, collation))
        {
            array_free_iterator(iterator);
            PG_RETURN_INT32(pos);
        }

        pos++;
    }

    array_free_iterator(iterator);
    PG_RETURN_INT32(-1);
}

Datum
array_last_index_of(PG_FUNCTION_ARGS)
{
    ArrayType      *array;
    Datum           searched_elem;
    Oid             elem_type;
	Oid             collation;
    ArrayIterator   iterator;
    Datum           element;
    bool            isnull;
    int32           pos;
    int32           position;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    array = PG_GETARG_ARRAYTYPE_P(0);

    if (ARR_NDIM(array) > 1)
    {
        ereport(ERROR, errmsg("multidimensional arrays is not supported"));
    }
    
	elem_type = ARR_ELEMTYPE(array);

    check_supported_type(elem_type);

    pos = 1;

    iterator = array_create_iterator(array, 0, NULL);

    position = -1;

    if (PG_ARGISNULL(1))
	{
        while (array_iterate(iterator, &element, &isnull))
        {
            if (isnull)
            {
                position = pos;
            }

            pos++;
        }

        array_free_iterator(iterator);
        PG_RETURN_INT32(position);
    }

	searched_elem  = PG_GETARG_DATUM(1);
	collation = PG_GET_COLLATION();

    while (array_iterate(iterator, &element, &isnull))
    {
        if (!isnull && !compare(elem_type, element, searched_elem, collation))
        {
            position = pos;
        }

        pos++;
    }

    array_free_iterator(iterator);
    PG_RETURN_INT32(position);
}
