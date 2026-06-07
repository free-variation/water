#include "logicforth.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

typedef pcre2_code *compiled_pattern_t;

#define REGEX_CACHE_SIZE 64

static struct {
	char *pattern;
	pcre2_code *re;
	int in_use;
} regex_cache[REGEX_CACHE_SIZE];

static int regex_cache_next;

static pcre2_code *compiled_pattern(Interpreter *interp, Object *pattern) {
	for (int i = 0; i < REGEX_CACHE_SIZE; i++)
		if (regex_cache[i].in_use && strcmp(regex_cache[i].pattern, pattern->bytes) == 0)
			return regex_cache[i].re;

	int errcode;
	PCRE2_SIZE erroffset;
	pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern->bytes, (PCRE2_SIZE)pattern->len,
			PCRE2_MULTILINE, &errcode, &erroffset, NULL);
	if (!re) {
		PCRE2_UCHAR message[256];
		pcre2_get_error_message(errcode, message, sizeof(message));
		fail(interp, "regex: %s", (char *)message);
		return NULL;
	}
	pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);

	int slot = regex_cache_next;
	regex_cache_next = (regex_cache_next + 1) % REGEX_CACHE_SIZE;
	if (regex_cache[slot].in_use) {
		pcre2_code_free(regex_cache[slot].re);
		free(regex_cache[slot].pattern);
	}
	regex_cache[slot].pattern = strdup(pattern->bytes);
	regex_cache[slot].re = re;
	regex_cache[slot].in_use = 1;
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

static int capture_array(Interpreter *interp, Object *subject, regmatch_t *offsets, int num_groups) {
	int handle = object_new_array(interp, num_groups);
	if (interp->error_flag) return -1;
	Object *match = interp->objects[handle];
	memset(match->items, 0, sizeof(Val) * (size_t)num_groups);
	gc_root_push(interp, make_array(handle));
	for (int i = 0; i < num_groups; i++) {
		if (offsets[i].rm_so < 0) {
			match->items[i] = make_float(0.0);
		} else {
			int start = (int)offsets[i].rm_so;
			int length = (int)(offsets[i].rm_eo - offsets[i].rm_so);
			match->items[i] = make_string(object_new_string(interp, subject->bytes + start, length));
		}
	}
	gc_root_pop(interp);
	return handle;
}

static int next_match(pcre2_code *compiled, const char *bytes, int length, int from,
		int num_groups, regmatch_t *match_offsets, pcre2_match_data *md) {
	int rc = pcre2_match(compiled, (PCRE2_SPTR)bytes, (PCRE2_SIZE)length,
			(PCRE2_SIZE)from, 0, md, NULL);
	if (rc < 0)
		return -1;
	PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(md);
	for (int i = 0; i < num_groups; i++) {
		if (ovector[2 * i] == PCRE2_UNSET) {
			match_offsets[i].rm_so = -1;
			match_offsets[i].rm_eo = -1;
		} else {
			match_offsets[i].rm_so = (regoff_t)ovector[2 * i];
			match_offsets[i].rm_eo = (regoff_t)ovector[2 * i + 1];
		}
	}
	int resume_from = (int)match_offsets[0].rm_eo;
	if (match_offsets[0].rm_eo == match_offsets[0].rm_so)
		resume_from = (int)match_offsets[0].rm_eo + 1;
	return resume_from;
}

int string_matches(Interpreter *interp, Object *subject, Object *pattern) {
	pcre2_code *compiled = compiled_pattern(interp, pattern);
	if (interp->error_flag) return 0;
	pcre2_match_data *md = pcre2_match_data_create(1, NULL);
	int rc = pcre2_match(compiled, (PCRE2_SPTR)subject->bytes, PCRE2_ZERO_TERMINATED, 0, 0, md, NULL);
	pcre2_match_data_free(md);
	return rc >= 0;
}

void p_match_all(Interpreter *interp) {
	PEEK_STRING_AT(pattern, 0, "match-all");
	PEEK_STRING_AT(subject, 1, "match-all");
	COMPILE_PATTERN(compiled, num_groups, pattern);

	pcre2_match_data *md = pcre2_match_data_create(num_groups, NULL);
	regmatch_t *spans = NULL;
	int count = 0, capacity = 0;
	for (int from = 0; from <= subject->len; ) {
		if (count + 1 > capacity) {
			capacity = capacity ? capacity * 2 : 16;
			spans = realloc(spans, (size_t)capacity * (size_t)num_groups * sizeof(regmatch_t));
		}
		int resume_from = next_match(compiled, subject->bytes, subject->len, from, num_groups, &spans[count * num_groups], md);
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
		int match_handle = capture_array(interp, subject, &spans[match_index * num_groups], num_groups);
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

	regmatch_t match_offsets[num_groups];
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

	regmatch_t match_offsets[num_groups];
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
		int piece_length = (int)match_offsets[0].rm_so - previous_end;
		result->items[piece_index++] = make_string(object_new_string(interp, subject->bytes + previous_end, piece_length));
		previous_end = (int)match_offsets[0].rm_eo;
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

static void append_bytes(char **buffer, int *length, int *capacity, const char *src, int n) {
	if (*length + n > *capacity) {
		while (*length + n > *capacity)
			*capacity = *capacity ? *capacity * 2 : 64;
		*buffer = realloc(*buffer, (size_t)*capacity);
	}
	memcpy(*buffer + *length, src, (size_t)n);
	*length += n;
}

void p_replace(Interpreter *interp) {
	PEEK_STRING_AT(replacement, 0, "replace");
	PEEK_STRING_AT(pattern, 1, "replace");
	PEEK_STRING_AT(subject, 2, "replace");
	COMPILE_PATTERN(compiled, num_groups, pattern);
	regmatch_t match_offsets[num_groups];
	pcre2_match_data *md = pcre2_match_data_create(num_groups, NULL);

	char *out = NULL;
	int length = 0, capacity = 0;
	int pos = 0;

	for (int from = 0; from <= subject->len; ) {
		int resume_from = next_match(compiled, subject->bytes, subject->len, from, num_groups, match_offsets, md);
		if (resume_from < 0) break;
		int start = (int)match_offsets[0].rm_so;
		int end = (int)match_offsets[0].rm_eo;

		append_bytes(&out, &length, &capacity, subject->bytes + pos, start - pos);

		for (int i = 0; i < replacement->len; ) {
			char c = replacement->bytes[i];
			if (c == '\\' && i + 1 < replacement->len) {
				char escaped = replacement->bytes[i + 1];
				if (escaped == '\\' || escaped == '&') {
					append_bytes(&out, &length, &capacity, &escaped, 1);
					i += 2;
				} else if (escaped >= '0' && escaped <= '9') {
					int group = escaped - '0';
					if (group >= num_groups) {
						free(out);
						pcre2_match_data_free(md);
						fail(interp, "replace: backref \\%d but pattern has %d group(s)", group, num_groups - 1);
						return;
					}
					if (match_offsets[group].rm_so >= 0)
						append_bytes(&out, &length, &capacity, subject->bytes + match_offsets[group].rm_so,
								(int)(match_offsets[group].rm_eo - match_offsets[group].rm_so));
					i += 2;
				} else {
					append_bytes(&out, &length, &capacity, &c, 1);
					i += 1;
				}
			} else if (c == '&') {
				append_bytes(&out, &length, &capacity, subject->bytes + match_offsets[0].rm_so,
						(int)(match_offsets[0].rm_eo - match_offsets[0].rm_so));
				i += 1;
			} else {
				append_bytes(&out, &length, &capacity, &c, 1);
				i += 1;
			}
		}

		pos = end;
		from = resume_from;
	}
	pcre2_match_data_free(md);
	append_bytes(&out, &length, &capacity, subject->bytes + pos, subject->len - pos);

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

	if (start < 0 || end > source->len || start > end) {
		fail(interp, "substring: range [%d, %d) out of bounds for length %d", start, end, source->len);
		return;
	}

	int handle = object_new_string(interp, source->bytes + start, end - start);
	interp->data_stack[interp->dsp - 1] = make_string(handle);

	DISPATCH(interp);
}

void p_join(Interpreter *interp) {
	PEEK_STRING_AT(separator, 0, "join");
	PEEK_TYPE_AT(array_val, 1, "join", T_ARRAY);
	Object *array = interp->objects[VAL_DATA(array_val)];

	int total = 0;
	for (int i = 0; i < array->len; i++) {
		if (VAL_TAG(array->items[i]) != T_STRING) {
			fail(interp, "join: element %d is %s, expected a string", i, tag_name(VAL_TAG(array->items[i])));
			return;
		}
		total += interp->objects[VAL_DATA(array->items[i])]->len;
	}
	if (array->len > 0)
		total += separator->len * (array->len - 1);

	int handle = object_new_string_uninit(interp, total);
	if (interp->error_flag) return;
	Object *result = interp->objects[handle];
	int offset = 0;
	for (int i = 0; i < array->len; i++) {
		if (i > 0) {
			memcpy(result->bytes + offset, separator->bytes, (size_t)separator->len);
			offset += separator->len;
		}
		Object *piece = interp->objects[VAL_DATA(array->items[i])];
		memcpy(result->bytes + offset, piece->bytes, (size_t)piece->len);
		offset += piece->len;
	}

	interp->data_stack[interp->dsp - 2] = make_string(handle);
	interp->dsp -= 1;

	DISPATCH(interp);
}
