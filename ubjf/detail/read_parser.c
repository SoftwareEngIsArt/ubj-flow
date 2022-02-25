//
// Created by switchblade on 2022-02-24.
//

#include "read.h"

#include <stdlib.h>

#include "error_handling.h"
#include "token.h"
#include "bswap.h"

/* Store all relevant data in a single structure for single pointer access. */
typedef struct
{
	jmp_buf *panic_buf;
	ubjf_error error;
	ubjf_read_event_info read_info;
	ubjf_parse_event_info parse_info;
	ubjf_highp_mode highp_mode;
	size_t total_nodes;
} ubjf_parse_ctx;

static const ubjf_type ubjf_token_type_map[UBJF_TOKEN_MAX] = {
		[UBJF_TOKEN_NULL]         = UBJF_NULL,
		[UBJF_TOKEN_NOOP]         = UBJF_NOOP,
		[UBJF_TOKEN_FALSE]        = UBJF_FALSE_TYPE,
		[UBJF_TOKEN_TRUE]         = UBJF_TRUE_TYPE,
		[UBJF_TOKEN_INT8]         = UBJF_INT8,
		[UBJF_TOKEN_UINT8]        = UBJF_UINT8,
		[UBJF_TOKEN_INT16]        = UBJF_INT16,
		[UBJF_TOKEN_INT32]        = UBJF_INT32,
		[UBJF_TOKEN_INT64]        = UBJF_INT64,
		[UBJF_TOKEN_FLOAT32]      = UBJF_FLOAT32,
		[UBJF_TOKEN_FLOAT64]      = UBJF_FLOAT64,
		[UBJF_TOKEN_HIGHP]        = UBJF_HIGHP,
		[UBJF_TOKEN_CHAR]         = UBJF_CHAR,
		[UBJF_TOKEN_STRING]       = UBJF_STRING,
		[UBJF_TOKEN_ARRAY_START]  = UBJF_ARRAY,
		[UBJF_TOKEN_OBJECT_START] = UBJF_OBJECT,
};

/* Read event invocation. */
static void ubjf_guarded_read(ubjf_parse_ctx *restrict ctx, void *restrict dest, size_t n)
{
	if (UBJF_UNLIKELY(!(ctx->read_info.read && ctx->read_info.read(dest, n, ctx->read_info.udata) == n)))
		THROW_ERROR(ctx->panic_buf, UBJF_EOF);
}
static char ubjf_guarded_peek(ubjf_parse_ctx *ctx)
{
	int result;
	if (UBJF_UNLIKELY(!(ctx->read_info.peek && (result = ctx->read_info.peek(ctx->read_info.udata)) != EOF)))
		THROW_ERROR(ctx->panic_buf, UBJF_EOF);
	return (char) result;
}
static void ubjf_guarded_bump(ubjf_parse_ctx *ctx, size_t n)
{
	if (UBJF_UNLIKELY(!(ctx->read_info.bump && ctx->read_info.bump(n, ctx->read_info.udata))))
		THROW_ERROR(ctx->panic_buf, UBJF_EOF);
}

static ubjf_token ubjf_read_token(ubjf_parse_ctx *ctx)
{
	ubjf_token token = 0;
	ubjf_guarded_read(ctx, &token, 1);
	return token;
}
static ubjf_token ubjf_peek_token(ubjf_parse_ctx *ctx) { return (ubjf_token) ubjf_guarded_peek(ctx); }

/* Parse event invocation. */
static void ubjf_invoke_on_value(ubjf_parse_ctx *ctx, ubjf_value value)
{
	if (UBJF_LIKELY(ctx->parse_info.on_value))
	{
		ubjf_error result = ctx->parse_info.on_value(value, ctx->parse_info.udata);
		if (result != UBJF_NO_ERROR)
			THROW_ERROR(ctx->panic_buf, result);
	}
}
static char *ubjf_invoke_string_alloc(ubjf_parse_ctx *ctx, size_t size)
{
	char *str;
	if (UBJF_LIKELY(ctx->parse_info.on_string_alloc))
		str = ctx->parse_info.on_string_alloc(size, ctx->parse_info.udata);
	else
		str = (char *) UBJF_MALLOC(size);

	if (UBJF_UNLIKELY(!str))
		THROW_ERROR(ctx->panic_buf, UBJF_ERROR_ALLOC);
	return str;
}
static void ubjf_invoke_on_container_begin(ubjf_parse_ctx *ctx, ubjf_type container_type, size_t length,
                                           ubjf_type value_type)
{
	if (UBJF_LIKELY(ctx->parse_info.on_container_begin))
	{
		ubjf_error result = ctx->parse_info.on_container_begin(container_type, length, value_type,
		                                                       ctx->parse_info.udata);
		if (result != UBJF_NO_ERROR)
			THROW_ERROR(ctx->panic_buf, result);
	}
}
static void ubjf_invoke_on_container_end(ubjf_parse_ctx *ctx)
{
	if (UBJF_LIKELY(ctx->parse_info.on_container_end))
	{
		ubjf_error result = ctx->parse_info.on_container_end(ctx->parse_info.udata);
		if (result != UBJF_NO_ERROR)
			THROW_ERROR(ctx->panic_buf, result);
	}
}

static void ubjf_parse_node(ubjf_parse_ctx *ctx, ubjf_type type);
static void ubjf_read_node_recursive(ubjf_parse_ctx *ctx);

static void ubjf_parse_integer(ubjf_parse_ctx *restrict ctx, ubjf_value *restrict value)
{
	switch (value->type)
	{
		case UBJF_INT8: /* Covered by uint8_t since both are the same length. */
		case UBJF_UINT8:
		{
			uint8_t temp;
			ubjf_guarded_read(ctx, &temp, sizeof(temp));
			value->uint8 = temp;
			break;
		}
		case UBJF_INT16:
		{
			int16_t temp;
			ubjf_guarded_read(ctx, &temp, sizeof(temp));
			value->int16 = FIX_ENDIANNESS_16(temp);
			break;
		}
		case UBJF_INT32:
		{
			int32_t temp;
			ubjf_guarded_read(ctx, &temp, sizeof(temp));
			value->int32 = FIX_ENDIANNESS_32(temp);
			break;
		}
		case UBJF_INT64:
		{
			int64_t temp;
			ubjf_guarded_read(ctx, &temp, sizeof(temp));
			value->int64 = FIX_ENDIANNESS_64(temp);
			break;
		}
	}
}
static void ubjf_parse_float(ubjf_parse_ctx *restrict ctx, ubjf_value *restrict value)
{
	switch (value->type)
	{
		case UBJF_FLOAT32:
		{
			float temp;
			ubjf_guarded_read(ctx, &temp, sizeof(temp));
			value->float32 = FIX_ENDIANNESS_32(temp);
			break;
		}
		case UBJF_FLOAT64:
		{
			double temp;
			ubjf_guarded_read(ctx, &temp, sizeof(temp));
			value->float64 = FIX_ENDIANNESS_64(temp);
			break;
		}
	}
}
static int64_t ubjf_parse_length(ubjf_parse_ctx *ctx)
{
	ubjf_value length = {.type = ubjf_token_type_map[ubjf_read_token(ctx)]};
	if (UBJF_UNLIKELY(!(length.type & UBJF_INTEGER_TYPE_MASK)))
		THROW_ERROR(ctx->panic_buf, UBJF_ERROR_BAD_DATA);

	/* Read the length and make sure it is within i64 range.
	 * Since all integer values are stored in a union,
	 * using int64 will cover all possible integer sizes. */
	ubjf_parse_integer(ctx, &length);
	if (length.int64 < 0)
		THROW_ERROR(ctx->panic_buf, UBJF_ERROR_BAD_DATA);

	return length.int64;
}
static void ubjf_parse_string(ubjf_parse_ctx *restrict ctx, ubjf_value *restrict value)
{
	int64_t length = ubjf_parse_length(ctx);
	char *str = ubjf_invoke_string_alloc(ctx, length + 1); /* Add 1 for null terminator. */

	/* Guard for string read. */
	GUARD_ERROR(ctx->panic_buf, ctx->error)
	{
		ubjf_guarded_read(ctx, str, length);
	} else RETHROW_ERROR(ctx->panic_buf, ctx->error);
	END_GUARD(ctx->panic_buf);

	/* If reached here, we have read a string now. */
	str[length] = '\0';
	value->string.data = str;
	value->string.size = length;
}
static void ubjf_skip_highp(ubjf_parse_ctx *ctx)
{
	int64_t length = ubjf_parse_length(ctx);
	ubjf_guarded_bump(ctx, length);
}
static void ubjf_parse_value(ubjf_parse_ctx *ctx, ubjf_type type)
{
	ubjf_value value = {.type = type};
	if ((type & UBJF_BOOL_TYPE_MASK) == UBJF_BOOL_TYPE_MASK)
	{
		value.boolean = type & 1;   /* Extract the value flag. */
		value.type = UBJF_BOOL;     /* Change to bool type. */
	} else if (type & UBJF_INTEGER_TYPE_MASK)
		ubjf_parse_integer(ctx, &value);
	else if (type & UBJF_FLOAT_TYPE_MASK)
		ubjf_parse_float(ctx, &value);
	else if (type == UBJF_TOKEN_STRING)
		ubjf_parse_string(ctx, &value);
	else if (type == UBJF_TOKEN_HIGHP)
	{
		if (ctx->highp_mode == UBJF_HIGHP_SKIP)
		{
			ubjf_skip_highp(ctx);
			return; /* Don't invoke value event  since wwe are skipping it. */
		} else if (ctx->highp_mode == UBJF_HIGHP_AS_STRING)
			ubjf_parse_string(ctx, &value);
		else
			THROW_ERROR(ctx->panic_buf, UBJF_ERROR_HIGHP);
	}

	ubjf_invoke_on_value(ctx, value);
}

static void ubjf_parse_sized_container_next(ubjf_parse_ctx *ctx, ubjf_type value_type)
{
	/* If the object is weakly-typed, do a full read, otherwise parse with type. */
	if (value_type != UBJF_NO_TYPE)
		ubjf_parse_node(ctx, value_type);
	else
		ubjf_read_node_recursive(ctx);
}
static void ubjf_parse_array(ubjf_parse_ctx *ctx, int64_t length, ubjf_type value_type)
{
	if (length >= 0)
		while (length-- > 0)
		{
			ubjf_parse_sized_container_next(ctx, value_type);
		}
	else
		for (;;)
		{
			ubjf_token token = ubjf_read_token(ctx);
			if (token == UBJF_TOKEN_ARRAY_END)
				break;

			/* Expect that it is a value token. */
			ubjf_type type = ubjf_token_type_map[token];
			if (UBJF_UNLIKELY(type == UBJF_NO_TYPE))
				THROW_ERROR(ctx->panic_buf, UBJF_ERROR_BAD_DATA);

			ubjf_parse_node(ctx, type);
		}
}
static void ubjf_parse_object(ubjf_parse_ctx *ctx, int64_t length, ubjf_type value_type)
{
	if (length >= 0)
		while (length-- > 0)
		{
			ubjf_parse_value(ctx, UBJF_STRING);
			ubjf_parse_sized_container_next(ctx, value_type);
		}
	else
		for (;;)
		{
			if (ubjf_guarded_peek(ctx) == UBJF_TOKEN_OBJECT_END)
			{
				ubjf_guarded_bump(ctx, 1);
				break;
			}

			/* Parse the string key and read the value node. */
			ubjf_parse_value(ctx, UBJF_STRING);
			ubjf_read_node_recursive(ctx);
		}
}
static void ubjf_parse_container_preface(ubjf_parse_ctx *restrict ctx,
                                         int64_t *restrict out_length,
                                         ubjf_type *restrict out_value_type)
{
	switch (ubjf_peek_token(ctx))
	{
		case UBJF_TOKEN_CONTAINER_TYPE:
		{
			/* If explicit type is specified, read the type, then read the length. */
			ubjf_guarded_bump(ctx, 1);

			*out_value_type = ubjf_token_type_map[ubjf_read_token(ctx)];
			if (UBJF_UNLIKELY(*out_value_type == UBJF_NO_TYPE))
				THROW_ERROR(ctx->panic_buf, UBJF_ERROR_BAD_DATA);

			/* For strongly typed containers, length always follows the type. */
			if (UBJF_UNLIKELY(ubjf_peek_token(ctx) != UBJF_TOKEN_CONTAINER_LENGTH))
				THROW_ERROR(ctx->panic_buf, UBJF_ERROR_BAD_DATA);
		}
		case UBJF_TOKEN_CONTAINER_LENGTH:
		{
			ubjf_guarded_bump(ctx, 1);
			*out_length = ubjf_parse_length(ctx);
		}
		default:
			break;
	}
}
static void ubjf_parse_container(ubjf_parse_ctx *ctx, ubjf_type type)
{
	int64_t length = -1; /* -1 means no fixed size. */
	ubjf_type value_type = UBJF_NO_TYPE;
	ubjf_parse_container_preface(ctx, &length, &value_type);

	ubjf_invoke_on_container_begin(ctx, type, length, value_type);

	/* Guard for container parse. */
	GUARD_ERROR(ctx->panic_buf, ctx->error)
	{
		if (type == UBJF_ARRAY)
			ubjf_parse_array(ctx, length, value_type);
		else
			ubjf_parse_object(ctx, length, value_type);
	} else RETHROW_ERROR(ctx->panic_buf, ctx->error);
	END_GUARD(ctx->panic_buf);
	ubjf_invoke_on_container_end(ctx);
}

static void ubjf_parse_node(ubjf_parse_ctx *ctx, ubjf_type type)
{
	if (type & UBJF_VALUE_TYPE_MASK)
		ubjf_parse_value(ctx, type);
	else if (type & UBJF_CONTAINER_TYPE_MASK)
		ubjf_parse_container(ctx, type);

	ctx->total_nodes++;
}
static void ubjf_read_node_recursive(ubjf_parse_ctx *ctx)
{
	ubjf_type type = ubjf_token_type_map[ubjf_read_token(ctx)];
	if (UBJF_UNLIKELY(type == UBJF_NO_TYPE))
		THROW_ERROR(ctx->panic_buf, UBJF_ERROR_BAD_DATA);

	ubjf_parse_node(ctx, type);
}

ubjf_error ubjf_read_next(ubjf_read_state *restrict state, size_t *restrict nodes)
{
#ifdef UBJF_DISABLE_CHECKS
	if (UBJF_UNLIKELY(!state))
		return UBJF_MAKE_PARAM_ERROR(0);
#endif

	ubjf_parse_ctx ctx = {
			.panic_buf      = NULL,
			.error          = UBJF_NO_ERROR,
			.read_info      = state->read_event_info,
			.parse_info     = state->parse_event_info,
			.highp_mode     = state->highp_mode,
			.total_nodes    = 0,
	};

	/* Do the actual reading. */
	GUARD_ERROR(ctx.panic_buf, ctx.error)
	{
		ubjf_read_node_recursive(&ctx);
		/* No need to end the guard, since ubjf_read_next is always top-level. */
	}

	UBJF_SET_OPTIONAL(nodes, ctx.total_nodes);
	return ctx.error;
}
