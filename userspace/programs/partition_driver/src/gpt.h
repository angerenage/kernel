#pragma once

struct block_device;

enum gpt_result {
	GPT_RESULT_VALID = 0u,
	GPT_RESULT_NOT_PRESENT,
	GPT_RESULT_INVALID,
	GPT_RESULT_IO_ERROR,
};

/* Validate a primary or backup GPT, including both mandated CRC32 values. */
enum gpt_result gpt_probe(const struct block_device* device);
