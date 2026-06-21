#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef struct
{
	uint64_t begin, end;
} SimpleIndexPair;

#define declare_simple_vector(name, type) \
	typedef struct \
	{ \
		uint64_t length; \
		uint64_t length_cap; \
		uint64_t _real_size; \
		type *data; \
	} name; \
	name name##_create(uint64_t length_cap); \
	void name##_destroy(name *v); \
	SimpleIndexPair name##_ensure(name *v, SimpleIndexPair range); \

#define define_simple_vector(name, type) \
	declare_simple_vector(name, type) \
	name name##_create(uint64_t length_cap) \
	{ \
		name v; \
		v.length = 0; \
		v.length_cap = length_cap; \
		v._real_size = 0; \
		v.data = NULL; \
		return v; \
	} \
	void name##_destroy(name *v) \
	{ \
		free(v->data); \
		v->data = NULL; \
		v->length = 0; \
		v->_real_size = 0; \
	} \
	SimpleIndexPair name##_ensure(name *v, SimpleIndexPair range) \
	{ \
		if (range.end <= v->_real_size) \
		{ \
			if (v->length < range.end) \
				v->length = range.end; \
			return range; \
		} \
		uint64_t prev_size = v->_real_size; \
		uint64_t natural_size = prev_size + (prev_size >> 1); \
		uint64_t new_size = range.end; \
		if (new_size < natural_size) \
			new_size = natural_size; \
		if (new_size > v->length_cap) \
			new_size = v->length_cap; \
		type *data_new = realloc(v->data, new_size * sizeof(type)); \
		if (data_new == NULL) \
		{ \
			new_size = prev_size; \
			data_new = v->data; \
		} \
		memset(data_new + prev_size, 0, (new_size - prev_size) * sizeof(type)); \
		v->data = data_new; \
		v->_real_size = new_size; \
		if (range.end > new_size) \
			range.end = new_size; \
		v->length = range.end; \
		return range; \
	}
