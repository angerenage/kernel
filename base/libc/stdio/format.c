#include "format.h"

#include <base/display.h>
#include <libc/limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISPLAY_BUFFER_SIZE 256u

struct format_ctx {
	char*  buffer;
	size_t capacity;
	size_t index;
	int    count;
	bool   bounded;
};

static void emit_char(struct format_ctx* ctx, char ch) {
	if (ctx->bounded) {
		if (ctx->capacity != 0u && ctx->index < ctx->capacity - 1u) ctx->buffer[ctx->index] = ch;
		ctx->index++;
		ctx->count++;
		return;
	}
	if (ctx->index == ctx->capacity) {
		base_display_write(ctx->buffer, ctx->index);
		ctx->index = 0u;
	}
	ctx->buffer[ctx->index++] = ch;
	ctx->count++;
}

static void emit_string(struct format_ctx* ctx, const char* str, int precision) {
	int emitted = 0;

	if (!str) {
		str = "(null)";
	}

	while (*str && (precision < 0 || emitted < precision)) {
		emit_char(ctx, *str++);
		emitted++;
	}
}

static void emit_padding(struct format_ctx* ctx, int count, char pad) {
	while (count-- > 0) {
		emit_char(ctx, pad);
	}
}

static void emit_unsigned_number(struct format_ctx* ctx, unsigned long long value, unsigned base, bool uppercase,
                                 int width, char pad) {
	char   buffer[32];
	size_t idx = 0;

	if (value == 0) {
		buffer[idx++] = '0';
	}
	else {
		while (value > 0) {
			unsigned digit = value % base;
			value /= base;
			if (digit < 10) {
				buffer[idx++] = (char)('0' + digit);
			}
			else {
				buffer[idx++] = (char)((uppercase ? 'A' : 'a') + (digit - 10));
			}
		}
	}

	int padding = width - (int)idx;
	if (padding > 0) {
		emit_padding(ctx, padding, pad);
	}

	while (idx > 0) {
		emit_char(ctx, buffer[--idx]);
	}
}

static void emit_signed_number(struct format_ctx* ctx, long long value, int width, bool zero_pad) {
	char               pad       = zero_pad ? '0' : ' ';
	bool               negative  = value < 0;
	unsigned long long magnitude = negative ? 0ull - (unsigned long long)value : (unsigned long long)value;
	char               buffer[32];
	size_t             idx = 0;

	if (magnitude == 0) {
		buffer[idx++] = '0';
	}
	else {
		while (magnitude > 0) {
			unsigned digit = magnitude % 10u;
			magnitude /= 10u;
			buffer[idx++] = (char)('0' + digit);
		}
	}

	int total_digits = (int)idx + (negative ? 1 : 0);
	int padding      = width - total_digits;

	if (negative && pad == '0') {
		emit_char(ctx, '-');
		negative = false;
	}

	if (padding > 0) {
		emit_padding(ctx, padding, pad);
	}

	if (negative) {
		emit_char(ctx, '-');
	}

	while (idx > 0) {
		emit_char(ctx, buffer[--idx]);
	}
}

#ifdef BASE_FORMAT_FLOAT
static void emit_general_float(struct format_ctx* ctx, double value, int width, int precision, bool zero_pad) {
	char   output[384];
	char   digits[17];
	size_t size     = 0u;
	bool   negative = __builtin_signbit(value);

	if (negative) value = -value;
	if (__builtin_isnan(value)) {
		output[size++] = 'n';
		output[size++] = 'a';
		output[size++] = 'n';
	}
	else if (__builtin_isinf(value)) {
		if (negative) output[size++] = '-';
		output[size++] = 'i';
		output[size++] = 'n';
		output[size++] = 'f';
	}
	else if (value == 0.0) {
		if (negative) output[size++] = '-';
		output[size++] = '0';
	}
	else {
		int    exponent   = 0;
		double normalized = value;
		if (precision < 0) precision = 6;
		if (precision == 0) precision = 1;
		if (precision > (int)sizeof(digits)) precision = (int)sizeof(digits);

		while (normalized >= 10.0) {
			normalized /= 10.0;
			exponent++;
		}
		while (normalized < 1.0) {
			normalized *= 10.0;
			exponent--;
		}
		double rounding = 0.5;
		for (int index = 1; index < precision; index++) rounding /= 10.0;
		normalized += rounding;
		if (normalized >= 10.0) {
			normalized /= 10.0;
			exponent++;
		}
		for (int index = 0; index < precision; index++) {
			int digit     = (int)normalized;
			digits[index] = (char)('0' + digit);
			normalized    = (normalized - (double)digit) * 10.0;
		}
		int significant = precision;
		while (significant > 1 && digits[significant - 1] == '0') significant--;
		if (negative) output[size++] = '-';
		if (exponent < -4 || exponent >= precision) {
			output[size++] = digits[0];
			if (significant > 1) {
				output[size++] = '.';
				for (int index = 1; index < significant; index++) output[size++] = digits[index];
			}
			output[size++]          = 'e';
			output[size++]          = exponent < 0 ? '-' : '+';
			unsigned exponent_value = exponent < 0 ? (unsigned)-exponent : (unsigned)exponent;
			char     exponent_digits[3];
			size_t   exponent_size = 0u;
			do {
				exponent_digits[exponent_size++] = (char)('0' + exponent_value % 10u);
				exponent_value /= 10u;
			} while (exponent_value != 0u);
			while (exponent_size < 2u) exponent_digits[exponent_size++] = '0';
			while (exponent_size != 0u) output[size++] = exponent_digits[--exponent_size];
		}
		else if (exponent >= 0) {
			for (int index = 0; index <= exponent; index++) output[size++] = index < precision ? digits[index] : '0';
			if (significant > exponent + 1) {
				output[size++] = '.';
				for (int index = exponent + 1; index < significant; index++) output[size++] = digits[index];
			}
		}
		else {
			output[size++] = '0';
			output[size++] = '.';
			for (int index = -1; index > exponent; index--) output[size++] = '0';
			for (int index = 0; index < significant; index++) output[size++] = digits[index];
		}
	}

	int padding = width - (int)size;
	if (padding > 0 && zero_pad && size != 0u && output[0] == '-') {
		emit_char(ctx, '-');
		emit_padding(ctx, padding, '0');
		for (size_t index = 1u; index < size; index++) emit_char(ctx, output[index]);
		return;
	}
	if (padding > 0) emit_padding(ctx, padding, zero_pad ? '0' : ' ');
	for (size_t index = 0u; index < size; index++) emit_char(ctx, output[index]);
}
#endif

static unsigned long long read_unsigned(va_list* args, int length) {
	switch (length) {
	case 2:
		return va_arg(*args, unsigned long long);
	case 1:
		return va_arg(*args, unsigned long);
	default:
		return va_arg(*args, unsigned int);
	}
}

static long long read_signed(va_list* args, int length) {
	switch (length) {
	case 2:
		return va_arg(*args, long long);
	case 1:
		return va_arg(*args, long);
	default:
		return va_arg(*args, int);
	}
}

static void format_internal(struct format_ctx* ctx, const char* format, va_list* args) {
	while (*format) {
		if (*format != '%') {
			emit_char(ctx, *format++);
			continue;
		}

		format++;
		if (*format == '%') {
			emit_char(ctx, '%');
			format++;
			continue;
		}

		bool zero_pad  = false;
		int  width     = 0;
		int  precision = -1;

		if (*format == '0') {
			zero_pad = true;
			format++;
		}

		while (*format >= '0' && *format <= '9') {
			width = width * 10 + (*format - '0');
			format++;
		}

		if (*format == '.') {
			format++;
			precision = 0;
			if (*format == '*') {
				precision = va_arg(*args, int);
				if (precision < 0) precision = -1;
				format++;
			}
			else {
				while (*format >= '0' && *format <= '9') {
					precision = precision * 10 + (*format - '0');
					format++;
				}
			}
		}

		int  length      = 0;
		bool length_size = false;
		if (*format == 'l') {
			format++;
			length = 1;
			if (*format == 'l') {
				length = 2;
				format++;
			}
		}
		else if (*format == 'z') {
			length_size = true;
			format++;
		}

		char spec = *format ? *format++ : '\0';
		switch (spec) {
		case 'c': {
			char ch = (char)va_arg(*args, int);
			emit_char(ctx, ch);
			break;
		}
		case 's': {
			const char* str = va_arg(*args, const char*);
			emit_string(ctx, str, precision);
			break;
		}
		case 'd':
		case 'i': {
			long long value;
			if (length_size) {
				ptrdiff_t diff_value = va_arg(*args, ptrdiff_t);
				value                = (long long)diff_value;
			}
			else {
				value = read_signed(args, length);
			}
			emit_signed_number(ctx, value, width, zero_pad);
			break;
		}
		case 'u': {
			unsigned long long value;
			if (length_size) {
				size_t size_value = va_arg(*args, size_t);
				value             = (unsigned long long)size_value;
			}
			else {
				value = read_unsigned(args, length);
			}
			emit_unsigned_number(ctx, value, 10u, false, width, zero_pad ? '0' : ' ');
			break;
		}
		case 'x':
		case 'X': {
			unsigned long long value;
			if (length_size) {
				size_t size_value = va_arg(*args, size_t);
				value             = (unsigned long long)size_value;
			}
			else {
				value = read_unsigned(args, length);
			}
			emit_unsigned_number(ctx, value, 16u, spec == 'X', width, zero_pad ? '0' : ' ');
			break;
		}
		case 'p': {
			uintptr_t value = (uintptr_t)va_arg(*args, void*);
			emit_char(ctx, '0');
			emit_char(ctx, 'x');
			emit_unsigned_number(ctx, value, 16u, false, width ? width : (int)(sizeof(uintptr_t) * 2), '0');
			break;
		}
#ifdef BASE_FORMAT_FLOAT
		case 'g':
		case 'G':
			emit_general_float(ctx, va_arg(*args, double), width, precision, zero_pad);
			break;
#endif
		default:
			emit_char(ctx, '%');
			if (spec) {
				emit_char(ctx, spec);
			}
			break;
		}
	}
}

int format_to_display(const char* format, va_list* args) {
	char              buffer[DISPLAY_BUFFER_SIZE];
	struct format_ctx ctx = {
		.buffer   = buffer,
		.capacity = sizeof(buffer),
		.index    = 0u,
		.count    = 0,
	};

	format_internal(&ctx, format, args);
	if (ctx.index != 0u) base_display_write(ctx.buffer, ctx.index);
	return ctx.count;
}

int format_to_buffer(char* buffer, const char* format, va_list* args) {
	return format_to_buffer_size(buffer, SIZE_MAX, format, args);
}

int format_to_buffer_size(char* buffer, size_t size, const char* format, va_list* args) {
	if (format == NULL || (buffer == NULL && size != 0u)) return -1;

	struct format_ctx ctx = {
		.buffer   = buffer,
		.capacity = size,
		.index    = 0u,
		.count    = 0,
		.bounded  = true,
	};

	format_internal(&ctx, format, args);
	if (size != 0u) ctx.buffer[ctx.index < size ? ctx.index : size - 1u] = '\0';
	return ctx.count;
}
