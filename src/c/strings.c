#include "logicforth.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

int utf8_codepoint_count(const char *bytes, int length) {
	int count = 0;
	for (int i = 0; i < length; i++)
		if ((bytes[i] & 0xC0) != 0x80)
			count++;
	return count;
}

static int utf8_byte_offset(const char *bytes, int length, int char_index) {
	int byte = 0;

	for (int seen = 0; seen < char_index && byte < length; seen++) {
		byte++;
		while (byte < length && (bytes[byte] & 0xC0) == 0x80)
			byte++;
	}

	return byte;
}

int utf8_encode(int codepoint, char *out) {
	if (codepoint < 0x80) {
		out[0] = (char)codepoint;
		return 1;
	}
	if (codepoint < 0x800) {
		out[0] = (char)(0xC0 | (codepoint >> 6));
		out[1] = (char)(0x80 | (codepoint & 0x3F));
		return 2;
	}
	if (codepoint < 0x10000) {
		out[0] = (char)(0xE0 | (codepoint >> 12));
		out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
		out[2] = (char)(0x80 | (codepoint & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (codepoint >> 18));
	out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
	out[3] = (char)(0x80 | (codepoint & 0x3F));
	return 4;
}

static int utf8_decode(const char *bytes, int length, int *codepoint) {
	unsigned char lead = (unsigned char)bytes[0];
	if (lead < 0x80) {
		*codepoint = lead;
		return 1;
	}

	int extra;
	int value;
	if ((lead & 0xE0) == 0xC0) {
		extra = 1;
		value = lead & 0x1F;
	} else if ((lead & 0xF0) == 0xE0) {
		extra = 2;
		value = lead & 0x0F;
	} else if ((lead & 0xF8) == 0xF0) {
		extra = 3;
		value = lead & 0x07;
	} else {
		*codepoint = lead;
		return 1;
	}

	for (int i = 1; i <= extra && i < length; i++)
		value = (value << 6) | (bytes[i] & 0x3F);
	*codepoint = value;
	
	return extra + 1;
}

static pcre2_code *compiled_pattern(Interpreter *interp, Object *pattern) {
	for (int i = 0; i < REGEX_CACHE_SIZE; i++)
		if (interp->regex_cache[i].in_use
				&& interp->regex_cache[i].pattern_len == pattern->len
				&& memcmp(interp->regex_cache[i].pattern, pattern->bytes, (size_t)pattern->len) == 0)
			return interp->regex_cache[i].re;

	int errcode;
	PCRE2_SIZE erroffset;
	pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern->bytes, (PCRE2_SIZE)pattern->len,
			PCRE2_MULTILINE | PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_INVALID_UTF, &errcode, &erroffset, NULL);
	if (!re) {
		PCRE2_UCHAR message[256];
		pcre2_get_error_message(errcode, message, sizeof(message));
		fail(interp, "regex: %s", (char *)message);
		return NULL;
	}
	pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);

	int slot = interp->regex_cache_next;
	interp->regex_cache_next = (interp->regex_cache_next + 1) % REGEX_CACHE_SIZE;
	if (interp->regex_cache[slot].in_use) {
		pcre2_code_free(interp->regex_cache[slot].re);
		free(interp->regex_cache[slot].pattern);
	}
	interp->regex_cache[slot].pattern = malloc((size_t)pattern->len);
	memcpy(interp->regex_cache[slot].pattern, pattern->bytes, (size_t)pattern->len);
	interp->regex_cache[slot].pattern_len = pattern->len;
	interp->regex_cache[slot].re = re;
	interp->regex_cache[slot].in_use = 1;
	return re;
}

static int group_count(pcre2_code *compiled) {
	uint32_t captures;
	pcre2_pattern_info(compiled, PCRE2_INFO_CAPTURECOUNT, &captures);
	return (int)captures + 1;
}

#define COMPILE_PATTERN(re, num_groups, pat) \
	pcre2_code *re = compiled_pattern(interp, pat); \
	if (interp->error_flag) return; \
	int num_groups = group_count(re)

static int capture_array(Interpreter *interp, Object *subject, const int *offsets, int num_groups) {
	int handle = object_new_array(interp, num_groups);
	if (interp->error_flag) return -1;
	Object *match = OBJECT_AT(handle);
	memset(match->items, 0, sizeof(Val) * (size_t)num_groups);
	gc_root_push(interp, make_array(handle));
	for (int i = 0; i < num_groups; i++) {
		int start = offsets[2 * i];
		if (start < 0) {
			match->items[i] = make_float(0.0);
		} else {
			int length = offsets[2 * i + 1] - start;
			match->items[i] = make_string(object_new_string(interp, subject->bytes + start, length));
		}
	}
	gc_root_pop(interp);
	return handle;
}

static int next_match(pcre2_code *compiled, const char *bytes, int length, int from,
		int num_groups, int *match_offsets, pcre2_match_data *md) {
	int rc = pcre2_match(compiled, (PCRE2_SPTR)bytes, (PCRE2_SIZE)length,
			(PCRE2_SIZE)from, 0, md, NULL);
	if (rc < 0)
		return -1;
	PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(md);
	for (int i = 0; i < num_groups; i++) {
		if (ovector[2 * i] == PCRE2_UNSET) {
			match_offsets[2 * i] = -1;
			match_offsets[2 * i + 1] = -1;
		} else {
			match_offsets[2 * i] = (int)ovector[2 * i];
			match_offsets[2 * i + 1] = (int)ovector[2 * i + 1];
		}
	}
	int match_start = match_offsets[0];
	int match_end = match_offsets[1];
	int resume_from = match_end;
	if (match_end == match_start)
		resume_from = match_end + 1;
	return resume_from;
}

int string_matches(Interpreter *interp, Object *subject, Object *pattern) {
	pcre2_code *compiled = compiled_pattern(interp, pattern);
	if (interp->error_flag) return 0;
	pcre2_match_data *md = pcre2_match_data_create(1, NULL);
	int rc = pcre2_match(compiled, (PCRE2_SPTR)subject->bytes, (PCRE2_SIZE)subject->len, 0, 0, md, NULL);
	pcre2_match_data_free(md);
	return rc >= 0;
}

void p_match_all(Interpreter *interp) {
	PEEK_STRING_AT(pattern, 0, "match-all");
	PEEK_STRING_AT(subject, 1, "match-all");
	COMPILE_PATTERN(compiled, num_groups, pattern);

	pcre2_match_data *md = pcre2_match_data_create(num_groups, NULL);
	int *spans = NULL;
	int count = 0, capacity = 0;
	for (int from = 0; from <= subject->len; ) {
		if (count + 1 > capacity) {
			capacity = capacity ? capacity * 2 : 16;
			int *grown = realloc(spans, (size_t)capacity * (size_t)num_groups * 2 * sizeof(int));
			if (!grown) {
				free(spans);
				pcre2_match_data_free(md);
				fail(interp, "match-all: out of memory");
				return;
			}
			spans = grown;
		}
		int resume_from = next_match(compiled, subject->bytes, subject->len, from, num_groups, &spans[count * num_groups * 2], md);
		if (resume_from < 0) break;
		count++;
		from = resume_from;
	}
	pcre2_match_data_free(md);

	if (count == 0) {
		free(spans);
		interp->dsp -= 2;
		push(interp, make_float(0.0));
		DISPATCH(interp);
	}

	NEW_ARRAY(result_handle, result, count);
	memset(result->items, 0, sizeof(Val) * (size_t)count);
	gc_root_push(interp, make_array(result_handle));

	for (int match_index = 0; match_index < count; match_index++) {
		int match_handle = capture_array(interp, subject, &spans[match_index * num_groups * 2], num_groups);
		if (interp->error_flag) {
			gc_root_pop(interp);
			free(spans);
			return;
		}
		result->items[match_index] = make_array(match_handle);
	}
	gc_root_pop(interp);
	free(spans);

	interp->dsp -= 2;
	push(interp, make_array(result_handle));

	DISPATCH(interp);
}

void p_match(Interpreter *interp) {
	PEEK_STRING_AT(pattern, 0, "match");
	PEEK_STRING_AT(subject, 1, "match");
	COMPILE_PATTERN(compiled, num_groups, pattern);

	int match_offsets[num_groups * 2];
	pcre2_match_data *md = pcre2_match_data_create(num_groups, NULL);
	int resume_from = next_match(compiled, subject->bytes, subject->len, 0, num_groups, match_offsets, md);
	pcre2_match_data_free(md);

	if (resume_from < 0) {
		interp->dsp -= 2;
		push(interp, make_float(0.0));
		DISPATCH(interp);
	}

	int handle = capture_array(interp, subject, match_offsets, num_groups);
	if (interp->error_flag) return;

	interp->dsp -= 2;
	push(interp, make_array(handle));

	DISPATCH(interp);
}

void p_split(Interpreter *interp) {
	PEEK_STRING_AT(pattern, 0, "split");
	PEEK_STRING_AT(subject, 1, "split");
	COMPILE_PATTERN(compiled, num_groups, pattern);

	int match_offsets[num_groups * 2];
	pcre2_match_data *md = pcre2_match_data_create(num_groups, NULL);

	int match_count = 0;
	for (int from = 0; from <= subject->len; ) {
		int resume_from = next_match(compiled, subject->bytes, subject->len, from, num_groups, match_offsets, md);
		if (resume_from < 0)
			break;
		match_count++;
		from = resume_from;
	}

	NEW_ARRAY(result_handle, result, match_count + 1);
	memset(result->items, 0, sizeof(Val) * (size_t)(match_count + 1));
	gc_root_push(interp, make_array(result_handle));

	int piece_index = 0;
	int previous_end = 0;
	for (int from = 0; from <= subject->len; ) {
		int resume_from = next_match(compiled, subject->bytes, subject->len, from, num_groups, match_offsets, md);
		if (resume_from < 0)
			break;
		int piece_length = match_offsets[0] - previous_end;
		result->items[piece_index++] = make_string(object_new_string(interp, subject->bytes + previous_end, piece_length));
		previous_end = match_offsets[1];
		from = resume_from;
	}
	result->items[piece_index] = make_string(object_new_string(interp, subject->bytes + previous_end, subject->len - previous_end));

	gc_root_pop(interp);
	pcre2_match_data_free(md);
	if (interp->error_flag)
		return;

	interp->dsp -= 2;
	push(interp, make_array(result_handle));

	DISPATCH(interp);
}

static void append_bytes(Interpreter *interp, char **buffer, int *length, int *capacity, const char *src, int n) {
	if (interp->error_flag)
		return;
	if (*length + n > *capacity) {
		while (*length + n > *capacity)
			*capacity = *capacity ? *capacity * 2 : 64;
		char *grown = realloc(*buffer, (size_t)*capacity);
		if (!grown) {
			free(*buffer);
			*buffer = NULL;
			fail(interp, "replace: out of memory");
			return;
		}
		*buffer = grown;
	}
	memcpy(*buffer + *length, src, (size_t)n);
	*length += n;
}

void p_replace(Interpreter *interp) {
	PEEK_STRING_AT(replacement, 0, "replace");
	PEEK_STRING_AT(pattern, 1, "replace");
	PEEK_STRING_AT(subject, 2, "replace");
	COMPILE_PATTERN(compiled, num_groups, pattern);
	int match_offsets[num_groups * 2];
	pcre2_match_data *md = pcre2_match_data_create(num_groups, NULL);

	char *out = NULL;
	int length = 0, capacity = 0;
	int pos = 0;

	for (int from = 0; from <= subject->len && !interp->error_flag; ) {
		int resume_from = next_match(compiled, subject->bytes, subject->len, from, num_groups, match_offsets, md);
		if (resume_from < 0) break;
		int start = match_offsets[0];
		int end = match_offsets[1];

		append_bytes(interp, &out, &length, &capacity, subject->bytes + pos, start - pos);

		for (int i = 0; i < replacement->len; ) {
			char c = replacement->bytes[i];
			if (c == '\\' && i + 1 < replacement->len) {
				char escaped = replacement->bytes[i + 1];
				if (escaped == '\\' || escaped == '&') {
					append_bytes(interp, &out, &length, &capacity, &escaped, 1);
					i += 2;
				} else if (escaped >= '0' && escaped <= '9') {
					int group = escaped - '0';
					if (group >= num_groups) {
						free(out);
						pcre2_match_data_free(md);
						fail(interp, "replace: backref \\%d but pattern has %d group(s)", group, num_groups - 1);
						return;
					}
					if (match_offsets[2 * group] >= 0)
						append_bytes(interp, &out, &length, &capacity, subject->bytes + match_offsets[2 * group],
								match_offsets[2 * group + 1] - match_offsets[2 * group]);
					i += 2;
				} else {
					append_bytes(interp, &out, &length, &capacity, &c, 1);
					i += 1;
				}
			} else if (c == '&') {
				append_bytes(interp, &out, &length, &capacity, subject->bytes + match_offsets[0],
						match_offsets[1] - match_offsets[0]);
				i += 1;
			} else {
				append_bytes(interp, &out, &length, &capacity, &c, 1);
				i += 1;
			}
		}

		pos = end;
		from = resume_from;
	}
	pcre2_match_data_free(md);
	append_bytes(interp, &out, &length, &capacity, subject->bytes + pos, subject->len - pos);

	if (interp->error_flag) {
		free(out);
		return;
	}

	int result = object_new_string(interp, out ? out : "", length);
	free(out);
	if (interp->error_flag) return;

	interp->dsp -= 3;
	push(interp, make_string(result));

	DISPATCH(interp);
}

void p_substring(Interpreter *interp) {
	POP_INT(end, "substring", "end");
	POP_INT(start, "substring", "start");
	PEEK_STRING_AT(source, 0, "substring");

	int char_count = utf8_codepoint_count(source->bytes, source->len);
	if (start < 0 || end > char_count || start > end) {
		fail(interp, "substring: range [%d, %d) out of bounds for length %d", start, end, char_count);
		return;
	}

	int start_byte = utf8_byte_offset(source->bytes, source->len, start);
	int end_byte = utf8_byte_offset(source->bytes, source->len, end);
	int handle = object_new_string(interp, source->bytes + start_byte, end_byte - start_byte);
	interp->data_stack[interp->dsp - 1] = make_string(handle);

	DISPATCH(interp);
}

void p_byte_substring(Interpreter *interp) {
      POP_INT(end, "byte-substring", "end");
      POP_INT(start, "byte-substring", "start");
      PEEK_STRING_AT(source, 0, "byte-substring");

      if (start < 0 || end > source->len || start > end) {
              fail(interp, "byte-substring: range [%d, %d) out of bounds for length %d", start, end, source->len);
              return;
      }

      int handle = object_new_string(interp, source->bytes + start, end - start);
      interp->data_stack[interp->dsp - 1] = make_string(handle);

      DISPATCH(interp);
}	

static Val produce_char_string(Interpreter *interp, Object *source, int start_byte, int end_byte) {
	return make_string(object_new_string(interp, source->bytes + start_byte, end_byte - start_byte));
}

static Val produce_codepoint(Interpreter *interp, Object *source, int start_byte, int end_byte) {
	(void)interp;
	(void)end_byte;
	int codepoint;
	utf8_decode(source->bytes + start_byte, source->len - start_byte, &codepoint);
	return make_float((double)codepoint);
}

static void char_index_op(Interpreter *interp, const char *op,
		Val (*produce)(Interpreter *, Object *, int start_byte, int end_byte)) {
	POP_INT(index, op, "index");
	PEEK_STRING_AT(source, 0, op);

	int char_count = utf8_codepoint_count(source->bytes, source->len);
	if (index < 0 || index >= char_count) {
		fail(interp, "%s: index %d out of bounds for length %d", op, index, char_count);
		return;
	}

	int start_byte = utf8_byte_offset(source->bytes, source->len, index);
	int end_byte = utf8_byte_offset(source->bytes, source->len, index + 1);
	Val result = produce(interp, source, start_byte, end_byte);
	if (interp->error_flag) return;

	interp->data_stack[interp->dsp - 1] = result;
}

void p_char_at(Interpreter *interp) {
	char_index_op(interp, "char-at", produce_char_string);

	DISPATCH(interp);
}

void p_codepoint_at(Interpreter *interp) {
	char_index_op(interp, "codepoint-at", produce_codepoint);

	DISPATCH(interp);
}

static void string_explode(Interpreter *interp, const char *op,
		Val (*produce)(Interpreter *, Object *, int start_byte, int end_byte)) {
	PEEK_STRING_AT(source, 0, op);

	int char_count = utf8_codepoint_count(source->bytes, source->len);
	int handle = object_new_array(interp, char_count);
	if (interp->error_flag) return;
	Object *result = OBJECT_AT(handle);
	memset(result->items, 0, sizeof(Val) * (size_t)char_count);
	gc_root_push(interp, make_array(handle));

	int start_byte = 0;
	for (int index = 0; index < char_count; index++) {
		int end_byte = start_byte + 1;
		while (end_byte < source->len && (source->bytes[end_byte] & 0xC0) == 0x80)
			end_byte++;
		result->items[index] = produce(interp, source, start_byte, end_byte);
		if (interp->error_flag) {
			gc_root_pop(interp);
			return;
		}
		start_byte = end_byte;
	}
	gc_root_pop(interp);

	interp->data_stack[interp->dsp - 1] = make_array(handle);
}

void p_string_to_chars(Interpreter *interp) {
	string_explode(interp, "string>chars", produce_char_string);

	DISPATCH(interp);
}

void p_string_to_codepoints(Interpreter *interp) {
	string_explode(interp, "string>codepoints", produce_codepoint);

	DISPATCH(interp);
}

void p_codepoint_to_char(Interpreter *interp) {
	POP_INT(code, "codepoint>char", "codepoint");
	if (code < 0 || code > 0x10FFFF) {
		fail(interp, "codepoint>char: codepoint %d out of range", code);
		return;
	}

	char encoded[4];
	int length = utf8_encode(code, encoded);
	push(interp, make_string(object_new_string(interp, encoded, length)));

	DISPATCH(interp);
}

void p_codepoints_to_string(Interpreter *interp) {
	POP_ARRAY(codes, "codespoints>string");

	int codepoint_count = codes->len;
	char *buffer = malloc((size_t)codepoint_count * 4 + 1);
	if (!buffer) {
		fail(interp, "codepoints>string: out of memory");
		return;
	}

	int offset = 0;
	for (int i = 0; i < codepoint_count; i++) {
		Val item = codes->items[i];
		if (VAL_TAG(item) != T_FLOAT) {
			free(buffer);
			fail(interp, "codepoints>string: element %d is not a number; got %s", i, tag_name(VAL_TAG(item)));
			return;
		}

		int code = (int)VAL_NUMBER(item);
		if (code < 0 || code > 0x10FFFF) {
			free(buffer);
			fail(interp, "codepoints>string: codepoint %d out of range at element %d", code, i);
			return;
		}

		offset += utf8_encode(code, buffer + offset);
	}

	int handle = object_new_string(interp, buffer, offset);
	free(buffer);
	push(interp, make_string(handle));

	DISPATCH(interp);
}

static inline int trim_is_ws(unsigned char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

void p_trim(Interpreter *interp) {
	PEEK_STRING_AT(source, 0, "trim");

	int start = 0;
	int end = source->len;
	while (start < end && trim_is_ws((unsigned char)source->bytes[start]))
		start++;
	while (end > start && trim_is_ws((unsigned char)source->bytes[end - 1]))
		end--;

	int handle = object_new_string(interp, source->bytes + start, end - start);
	interp->data_stack[interp->dsp - 1] = make_string(handle);

	DISPATCH(interp);
}

void p_string_to_number(Interpreter *interp) {
	POP_STRING(source, "string>number");

	const char *text = source->bytes;
	char *end;
	double value = strtod(text, &end);

	if (end == text) {
		push(interp, make_tagged(T_NONE, 0));
		DISPATCH(interp);
	}

	while (isspace((unsigned char)*end))
		end++;

	if (*end != 0)
		push(interp, make_tagged(T_NONE, 0));
	else
		push(interp, make_float(value));

	DISPATCH(interp);
}

void p_join(Interpreter *interp) {
	PEEK_STRING_AT(separator, 0, "join");
	PEEK_TYPE_AT(array_val, 1, "join", T_ARRAY);
	Object *array = OBJECT_AT(VAL_DATA(array_val));

	int64_t total = 0;
	for (int i = 0; i < array->len; i++) {
		if (VAL_TAG(array->items[i]) != T_STRING) {
			fail(interp, "join: element %d is %s, expected a string", i, tag_name(VAL_TAG(array->items[i])));
			return;
		}
		total += OBJECT_AT(VAL_DATA(array->items[i]))->len;
	}
	if (array->len > 0)
		total += (int64_t)separator->len * (array->len - 1);
	if (total > INT_MAX) {
		fail(interp, "join: result too large (%lld bytes)", (long long)total);
		return;
	}

	int handle = object_new_string_uninit(interp, (int)total);
	if (interp->error_flag) return;
	Object *result = OBJECT_AT(handle);
	int offset = 0;
	for (int i = 0; i < array->len; i++) {
		if (i > 0) {
			memcpy(result->bytes + offset, separator->bytes, (size_t)separator->len);
			offset += separator->len;
		}
		Object *piece = OBJECT_AT(VAL_DATA(array->items[i]));
		memcpy(result->bytes + offset, piece->bytes, (size_t)piece->len);
		offset += piece->len;
	}

	interp->data_stack[interp->dsp - 2] = make_string(handle);
	interp->dsp -= 1;

	DISPATCH(interp);
}
