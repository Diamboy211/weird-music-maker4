#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef struct
{
	int64_t begin, end;
} IndexPair;

#define vector_at(_v, _i) ((_v)->_data[-(_v)->_begin + (_i)])

#define declare_vector(_name, _type) \
	typedef struct \
	{ \
		int64_t begin, end; \
		int64_t _begin, _end; \
		int64_t left_cap, right_cap; \
		_type *_data; \
	} _name; \
	_name _name##_create(int64_t _left_cap, int64_t _right_cap); \
	void _name##_destroy(_name *_v); \
	IndexPair _name##_ensure(_name *_v, IndexPair _range);

#define define_vector(_name, _type, _def) \
	declare_vector(_name, _type) \
	_name _name##_create(int64_t _left_cap, int64_t _right_cap) \
	{ \
		_name _v; \
		_v.left_cap = _left_cap; \
		_v.right_cap = _right_cap; \
		_v.begin = _v.end = _left_cap; \
		_v._begin = _v._end = _left_cap; \
		_v._data = NULL; \
		return _v; \
	} \
	void _name##_destroy(_name *_v) \
	{ \
		free(_v->_data); \
		memset(_v, 0, sizeof(_name)); \
	} \
	void _name##_clear(_name *_v) \
	{ \
		free(_v->_data); \
		_v->begin = _v->end = _left_cap; \
		_v->_begin = _v->_end = _left_cap; \
		_v->_data = NULL; \
	} \
	IndexPair _name##_ensure(_name *_v, IndexPair _range) \
	{ \
		if (_range.begin < _v->left_cap) _range.begin = _v->left_cap; \
		if (_range.end > _v->right_cap) _range.end = _v->right_cap; \
		if (_range.begin >= _range.end) return _range; \
		if (_v->_begin >= _v->_end) \
		{ \
			int64_t _nl = _range.end - _range.begin; \
			int64_t _n_begin = _range.begin; \
			int64_t _n_end = _range.end; \
			if (_nl < 8) \
			{ \
				_n_begin -= (8 - _nl) / 2; \
				if (_n_begin < _v->left_cap) _n_begin = _v->left_cap; \
				_n_end += (8 - _nl) - (8 - _nl) / 2; \
				if (_n_end > _v->right_cap) _n_end = _v->right_cap; \
				_nl = _n_end - _n_begin; \
			} \
			_v->_data = realloc(NULL, sizeof(_type) * _nl); \
			if (_v->_data == NULL) \
			{ \
				_range.begin = _range.end; \
				return _range; \
			} \
			for (_type *_it = _v->_data; _it < _v->_data + _nl; _it++) \
				*_it = (_def); \
			_v->begin = _range.begin; \
			_v->end = _range.end; \
			_v->_begin = _n_begin; \
			_v->_end = _n_end; \
			return _range; \
		} \
		int64_t _nbegin = _v->_begin, _nend = _v->_end; \
		int _exl, _exr; \
		if ((_exl = _nbegin > _range.begin)) _nbegin = _range.begin; \
		if ((_exr = _nend < _range.end)) _nend = _range.end; \
		if (!_exl && !_exr) \
		{ \
			if (_v->begin > _range.begin) _v->begin = _range.begin; \
			if (_v->end < _range.end) _v->end = _range.end; \
			return _range; \
		} \
		int64_t _pl = _v->_end - _v->_begin; \
		int64_t _n_begin = _v->_begin; \
		if (_exl) _n_begin -= _pl / 2; \
		int64_t _n_end = _v->_end; \
		if (_exr) _n_end += _pl - _pl / 2; \
		if (_n_begin > _nbegin) _n_begin = _nbegin; \
		if (_n_end < _nend) _n_end = _nend; \
		if (_n_begin < _v->left_cap) _n_begin = _v->left_cap; \
		if (_n_end > _v->right_cap) _n_end = _v->right_cap; \
		int64_t _nl = _n_end - _n_begin; \
		_type *_ndata = realloc(_v->_data, sizeof(_type) * _nl); \
		if (_ndata == NULL) \
		{ \
			if (_range.begin < _v->_begin) _range.begin = _v->_begin; \
			if (_range.end > _v->_end) _range.end = _v->_end; \
			if (_range.begin >= _range.end) return _range; \
			if (_v->begin > _range.begin) _v->begin = _range.begin; \
			if (_v->end < _range.end) _v->end = _range.end; \
			return _range; \
		} \
		_v->_data = _ndata; \
		_type *_d = _v->_data + _nl; \
		_type *_c = _v->_data + (_v->end - _n_begin); \
		_type *_b = _v->_data + (_v->end - _v->_begin); \
		_type *_a = _v->_data + (_v->begin - _v->_begin); \
		for (_type *_it = _c; _it < _d;) *_it++ = (_def); \
		if (_b < _c) \
		{ \
			int64_t _s = _b - _a; \
			memmove((_c -= _s), (_b -= _s), sizeof(_type) * _s); \
			while (_a < _c) *_a++ = (_def); \
		} \
		_v->_begin = _n_begin; \
		_v->_end = _n_end; \
		if (_v->begin > _range.begin) _v->begin = _range.begin; \
		if (_v->end < _range.end) _v->end = _range.end; \
		return _range; \
	}
