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

typedef enum {
    TT_IDENT,
    TT_INT,
    TT_NUM,
    TT_STRING,
    TT_NAN,
    TT_INF,
    TT_N_INF,
    TT_EQ_OP,
    TT_N_EQ_OP,
    TT_GR_OP,
    TT_EQ_GR_OP,
    TT_LE_OP,
    TT_EQ_LE_OP,
    TT_MOD_OP,
    TT_INVALID,
    TT_EOF
} token_type;

typedef struct {
    const char* str;
    int len;
    token_type type;
} token;

typedef struct {
    const char *data;
    int len;
    int pos;
} lexer;

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

static void lexer_init(lexer *lex, const char *data, int len)
{
    lex->data = data;
    lex->len = len;
    lex->pos = 0;
}

static void lexer_skip_whitespace(lexer *lex)
{
    char ch;

    while (lex->pos < lex->len)
    {
        ch = lex->data[lex->pos];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
        {
            lex->pos++;
        }
        else
        {
            break;
        }
    }
}

static bool lexer_next_token_operation(token* tkn, lexer* lex)
{
    tkn->str = lex->data + lex->pos;

    switch (lex->data[lex->pos])
    {
    case '=':
        if (lex->pos + 1 < lex->len && lex->data[lex->pos + 1] == '=')
        {
            lex->pos += 2;
            tkn->len = 2;
        }
        else
        {
            lex->pos += 1;
            tkn->len = 1;
        }
        tkn->type = TT_EQ_OP;
        return 1;
    case '!':
        if (lex->pos + 1 < lex->len && lex->data[lex->pos + 1] != '=')
        {
            return 0;
        }
        lex->pos += 2;
        tkn->len = 2;
        tkn->type = TT_N_EQ_OP;
        return 1;
    case '>':
        if (lex->pos + 1 < lex->len && lex->data[lex->pos + 1] == '=')
        {
            lex->pos += 2;
            tkn->len = 2;
            tkn->type = TT_EQ_GR_OP;
        }
        else
        {
            lex->pos += 1;
            tkn->len = 1;
            tkn->type = TT_GR_OP;
        }
        return 1;
    case '<':
        if (lex->pos + 1 < lex->len && lex->data[lex->pos + 1] == '=')
        {
            lex->pos += 2;
            tkn->len = 2;
            tkn->type = TT_EQ_LE_OP;
        }
        else
        {
            lex->pos += 1;
            tkn->len = 1;
            tkn->type = TT_LE_OP;
        }
        return 1;
    case '%':
        lex->pos += 1;
        tkn->len = 1;
        tkn->type = TT_MOD_OP;
        return 1;
    default:
        return 0;
    }
    return 0;
}

static bool lexer_next_token_string(token* tkn, lexer* lex)
{
    size_t string_len = 0;

    if (lex->data[lex->pos] != '\"')
    {
        return 0;
    }

    tkn->str = lex->data + lex->pos;
    string_len++;

    while (lex->pos + string_len < lex->len && lex->data[lex->pos + string_len] != '"')
    {
        string_len++;
    }

    if (lex->pos + string_len >= lex->len)
    {
        return 0;
    }

    string_len++;

    tkn->str = lex->data + lex->pos;
    tkn->len = string_len;
    tkn->type = TT_STRING;

    lex->pos += string_len;

    return 1;
}

static bool lexer_next_token_numeric(token* tkn, lexer* lex)
{
    size_t numeric_len = 0;
    bool has_digits = false;
    size_t first_digit = 0;

    if (lex->data[lex->pos] == '-') {
        first_digit = 1;
        numeric_len = 1;
    }

    if (lex->pos + numeric_len >= lex->len)
    {
        return 0;
    }

    while (lex->pos + numeric_len < lex->len &&
            lex->data[lex->pos + numeric_len] >= '0' && lex->data[lex->pos + numeric_len] <= '9')
    {
        numeric_len++;
        has_digits = 1;
    }

    if (numeric_len - first_digit > 1 && lex->data[lex->pos + first_digit] == '0')
    {
        return 0;
    }

    if (lex->pos + numeric_len < lex->len && lex->data[lex->pos + numeric_len] == '.')
    {
        numeric_len++;
    }
    else
    {
        if (!has_digits)
        {
            return 0;
        }

        tkn->len = numeric_len;
        tkn->str = lex->data + lex->pos;
        tkn->type = TT_INT;

        lex->pos += numeric_len;

        return 1;
    }

    while (lex->pos + numeric_len < lex->len &&
            lex->data[lex->pos + numeric_len] >= '0' && lex->data[lex->pos + numeric_len] <= '9')
    {
        numeric_len++;
        has_digits = 1;
    }

    if (!has_digits)
    {
        return 0;
    }

    tkn->len = numeric_len;
    tkn->str = lex->data + lex->pos;
    tkn->type = TT_NUM;

    lex->pos += numeric_len;

    return 1;
}

static bool lexer_next_token_identifier(token* tkn, lexer* lex)
{
    size_t ident_len = 0;

    while (lex->pos + ident_len < lex->len &&
            lex->data[lex->pos + ident_len] >= 'a' && lex->data[lex->pos + ident_len] <= 'z')
    {
        ident_len++;
    }

    if (ident_len == 0)
    {
        return 0;
    }

    tkn->str = lex->data + lex->pos;
    tkn->len = ident_len;
    tkn->type = TT_IDENT;

    lex->pos += ident_len;

    return 1;
}

static bool lexer_next_token_special(token* tkn, lexer* lex)
{
    size_t special_len = 0;

    while (lex->pos + special_len < lex->len &&
            ((lex->data[lex->pos + special_len] >= 'a' && lex->data[lex->pos + special_len] <= 'z') ||
            (lex->data[lex->pos + special_len] >= 'A' && lex->data[lex->pos + special_len] <= 'Z') ||
            lex->data[lex->pos + special_len] == '-'))
    {
        special_len++;
    }

    if (special_len == 3 && pg_strncasecmp(lex->data + lex->pos, "nan", 3) == 0) {
        tkn->type = TT_NAN;
    } else if (special_len == 8 && pg_strncasecmp(lex->data + lex->pos, "infinity", 8) == 0) {
        tkn->type = TT_INF;
    } else if (special_len == 3 && pg_strncasecmp(lex->data + lex->pos, "inf", 3) == 0) {
        tkn->type = TT_INF;
    } else if (special_len == 9 && pg_strncasecmp(lex->data + lex->pos, "-infinity", 9) == 0) {
        tkn->type = TT_N_INF;
    } else if (special_len == 4 && pg_strncasecmp(lex->data + lex->pos, "-inf", 4) == 0) {
        tkn->type = TT_N_INF;
    } else {
        return 0;
    }

    tkn->str = lex->data + lex->pos;
    tkn->len = special_len;

    lex->pos += special_len;

    return 1;
}

static token lexer_next_token(lexer *lex) {
    token result;
    char ch;

    lexer_skip_whitespace(lex);

    if (lex->pos >= lex->len)
    {
        result.str = lex->data + lex->pos;
        result.len = 0;
        result.type = TT_EOF;
        
        return result;
    }

    if (lexer_next_token_operation(&result, lex))
    {
        return result;
    }

    if (lexer_next_token_string(&result, lex))
    {
        return result;
    }

    if (lexer_next_token_numeric(&result, lex))
    {
        return result;
    }

    if (lexer_next_token_special(&result, lex))
    {
        return result;
    }

    if (lexer_next_token_identifier(&result, lex))
    {
        return result;
    }

    result.str = lex->data + lex->pos;
    result.type = TT_INVALID;
    result.len = 0;
    while (lex->pos < lex->len)
    {
        ch = lex->data[lex->pos];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
        {
            break;
        }
        else
        {
            result.len++;
            lex->pos++;
        }
    }

    return result;
}

static Datum datum_from_token(token tkn, Oid target_type)
{
    Datum item;
    char *buf;

    if (tkn.type != TT_STRING && is_string_type_supported(target_type))
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Expected a string value, got %.*s.", tkn.len, tkn.str),
                        errhint("String constants must be enclosed in double quotes, for example \'x == \"abc\"\'.")));
    }

    if (tkn.type != TT_INT && is_int_type_supported(target_type))
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Expected an integer value, got %.*s.", tkn.len, tkn.str)));
    }

    if (tkn.type != TT_INT && tkn.type != TT_NUM && tkn.type != TT_NAN &&
        tkn.type != TT_INF && tkn.type != TT_N_INF && is_numeric_type_supported(target_type))
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Expected a numeric value, got %.*s.", tkn.len, tkn.str)));
    }

    buf = (char*)palloc(sizeof(char)*(tkn.len+1));
    memcpy(buf, tkn.str, tkn.len*sizeof(char));
    buf[tkn.len] = '\0';

    switch (target_type)
    {
    case TEXTOID:
        buf[tkn.len - 1] = '\0';
        item = DirectFunctionCall1(textin, CStringGetDatum(buf + 1));
        break;

    case VARCHAROID:
        buf[tkn.len - 1] = '\0';
        item = DirectFunctionCall1(textin, CStringGetDatum(buf + 1));
        break;

    case BPCHAROID:
        buf[tkn.len - 1] = '\0';
        item = DirectFunctionCall3(bpcharin, CStringGetDatum(buf + 1), ObjectIdGetDatum(BPCHAROID), Int32GetDatum(-1));
        break;
    
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
                errdetail("Array element type %s is not supported.",
                    format_type_be(target_type))));
    }

    pfree(buf);
    return item;
}

static predicate parse_predicate(const char *data, int len, Oid target_type)
{
    predicate pred;
    token curr_tkn;
    lexer lex;

    pred.obj_type = target_type;

    lexer_init(&lex, data, len);
    
    curr_tkn = lexer_next_token(&lex);
    
    if (curr_tkn.type == TT_EOF)
    {
        pred.op = NONE_OP;
        return pred;
    }

    if (curr_tkn.type != TT_IDENT)
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Expected a variable name.")));
    }

    curr_tkn = lexer_next_token(&lex);

    switch (curr_tkn.type)
    {
    case TT_EQ_OP:
        pred.op = EQ_OP;
        break;
    case TT_N_EQ_OP:
        pred.op = N_EQ_OP;
        break;
    case TT_LE_OP:
        pred.op = LE_OP;
        break;
    case TT_EQ_LE_OP:
        pred.op = EQ_LE_OP;
        break;
    case TT_GR_OP:
        pred.op = GR_OP;
        break;
    case TT_EQ_GR_OP:
        pred.op = EQ_GR_OP;
        break;
    case TT_MOD_OP:
        pred.op = MOD_OP;
        break;
    case TT_EOF:
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Expected an operation, but reached the end of the expression.")));
        break;
    default:
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Expected an operation, got %.*s.", curr_tkn.len, curr_tkn.str)));
    }

    if (is_string_type_supported(target_type) && pred.op != EQ_OP && pred.op != N_EQ_OP)
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("String types support equality operators only.")));
    }
    
    if (pred.op == MOD_OP && !is_int_type_supported(target_type))
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Operator '%%' can only be used with integer array types.")));
    }

    curr_tkn = lexer_next_token(&lex);

    if (curr_tkn.type == TT_EOF)
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Expected a constant value, but reached the end of the expression.")));
    }

    pred.obj1 = datum_from_token(curr_tkn, target_type);

    curr_tkn = lexer_next_token(&lex);

    if (curr_tkn.type == TT_EOF)
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

    if (curr_tkn.type != TT_EQ_OP)
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Expected '=' operator.")));
    }

    curr_tkn = lexer_next_token(&lex);

    if (curr_tkn.type == TT_EOF)
    {
        ereport(ERROR, (errmsg("incorrect predicate syntax"),
                        errdetail("Expected a constant value, but reached the end of the expression.")));
    }

    pred.obj2 = datum_from_token(curr_tkn, target_type);

    curr_tkn = lexer_next_token(&lex);

    if (curr_tkn.type != TT_EOF)
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

    pred_text = PG_GETARG_TEXT_PP(1);
    pred_buf_len = VARSIZE_ANY_EXHDR(pred_text);
    pred_buf = VARDATA_ANY(pred_text);
    pred = parse_predicate(pred_buf, pred_buf_len, elem_type);
	
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

    pred_text = PG_GETARG_TEXT_PP(1);
    pred_buf_len = VARSIZE_ANY_EXHDR(pred_text);
    pred_buf = VARDATA_ANY(pred_text);
    pred = parse_predicate(pred_buf, pred_buf_len, elem_type);
	
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

    pred_text = PG_GETARG_TEXT_PP(1);
    pred_buf_len = VARSIZE_ANY_EXHDR(pred_text);
    pred_buf = VARDATA_ANY(pred_text);
    pred = parse_predicate(pred_buf, pred_buf_len, elem_type);
	
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
