//
// Created by switchblade on 2022-02-24.
//

#pragma once

typedef enum
{
	UBJF_BAD_TYPE = 0,
	UBJF_NULL,
	UBJF_NOOP,
	UBJF_BOOL,
	UBJF_CHAR,
	UBJF_INT8,
	UBJF_UINT8,
	UBJF_INT16,
	UBJF_INT32,
	UBJF_INT64,
	UBJF_FLOAT32,
	UBJF_FLOAT64,
	UBJF_HIGHP,
	UBJF_STRING,
	UBJF_ARRAY,
	UBJF_OBJECT,
} ubjf_type;