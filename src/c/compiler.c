/* Compile-time words: colon/quotation definition, control flow (if/begin/
   while/repeat), locals declaration, to/constant/variable/symbol, tick,
   inline, forget. Split out of words.c; all dependencies are in water.h. */
#include "water.h"

static void enter_compile_scope(Interpreter *interp);
static void leave_compile_scope(Interpreter *interp);

void rollback_partial_definition(void) {
	if (compiler.compiling_src_start <= 0 || vocab.latest_cfa == 0)
		return;
	int partial_cfa = vocab.latest_cfa;
	vocab.here = partial_cfa - 4;
	vocab.names_here = (int)WORD_NAME(partial_cfa);
	vocab.latest_cfa = (int)WORD_LINK(partial_cfa);
	truncate_quotation_spans();
	compiler.compiling = 0;
	compiler.compiling_src_start = 0;
	compiler.n_local_scopes = 0;
	compiler.n_local_names = 0;
	compiler.local_names_pool_here = 0;
	compiler.loop_begin = 0;
	compiler.leave_chain = 0;
}

void p_semicolon(DISPATCH_ARGS) {
	if (compiler.compiling_src_start > 0 && compiler.n_local_scopes > 1) {
		rollback_partial_definition();
		fail(interp, "; : unterminated quotation (a [: , [> , or [| has no matching :])");
		return;
	}
	if (compiler.loop_begin != 0) {
		rollback_partial_definition();
		fail(interp, "; : unterminated loop (a begin has no until/again/repeat)");
		return;
	}
	leave_compile_scope(interp);
	emit_call(interp, vocab.exit_cfa);
	if (compiler.compiling_src_start > 0 && vocab.latest_cfa != 0) {
		int src_end = compiler.input_buffer_pos - 1;
		int src_len = src_end - compiler.compiling_src_start;
		src_len = MAX(src_len, 0);
		if (vocab.source_here + src_len + 1 > SOURCE_POOL) {
			fail(interp, "source pool full (max %d bytes); definition source too large to store", SOURCE_POOL);
		} else {
			int source_offset = vocab.source_here;
			memcpy(&vocab.source_pool[vocab.source_here],
					&compiler.input_buffer[compiler.compiling_src_start],
					(size_t)src_len);
			vocab.source_pool[vocab.source_here + src_len] = 0;
			vocab.source_here += src_len + 1;
			WORD_SOURCE(vocab.latest_cfa) = source_offset;
		}
	}
	compiler.compiling = 0;
	compiler.compiling_src_start = 0;

	DISPATCH(interp);
}

static int try_fuse_cmp_branch(Interpreter *interp) {
	int fused_cfa;

	if (compiler.fuse_prev_cmp == vocab.eq_cfa)
		fused_cfa = vocab.eq_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.lt_cfa)
		fused_cfa = vocab.lt_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.gt_cfa)
		fused_cfa = vocab.gt_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.zeq_cfa)
		fused_cfa = vocab.zeq_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.eq_f_cfa)
		fused_cfa = vocab.eq_f_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.lt_f_cfa)
		fused_cfa = vocab.lt_f_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.gt_f_cfa)
		fused_cfa = vocab.gt_f_zbranch_cfa;
	else
		return 0;

	vocab.here--;
	emit_call(interp, fused_cfa);
	return 1;
}


void p_if(DISPATCH_ARGS) {
	if (!try_fuse_cmp_branch(interp))
		emit_call(interp, vocab.zbranch_cfa);
	push(interp, make_float((double)vocab.here));
	emit(interp, 0);

	DISPATCH(interp);
}

void p_qif(DISPATCH_ARGS) {
	emit_call(interp, vocab.qzbranch_cfa);
	push(interp, make_float((double)vocab.here));
	emit(interp, 0);

	DISPATCH(interp);
}

static int valid_patch_slot(Interpreter *interp, int slot, const char *op) {
	if (slot < DICT_RESERVED || slot > vocab.here) {
		fail(interp, "%s: no matching control-flow opener", op);
		return 0;
	}
	return 1;
}

void p_then(DISPATCH_ARGS) {
	POP(slot_val);
	int slot = (int)VAL_NUMBER(slot_val);
	if (!valid_patch_slot(interp, slot, "then"))
		return;
	vocab.dict[slot] = (vocab.here - slot);
	compiler.fuse_floor = vocab.here;

	DISPATCH(interp);
}

void p_else(DISPATCH_ARGS) {
	POP(slot_val);
	int slot = (int)VAL_NUMBER(slot_val);
	if (!valid_patch_slot(interp, slot, "else"))
		return;
	emit_call(interp, vocab.branch_cfa);
	push(interp, make_float((double)vocab.here));
	emit(interp, 0);
	vocab.dict[slot] = (vocab.here - slot);

	DISPATCH(interp);
}

void p_begin(DISPATCH_ARGS) {
	push(interp, make_float((double)compiler.loop_begin));
	push(interp, make_float((double)compiler.leave_chain));
	push(interp, make_float((double)vocab.here));
	compiler.loop_begin = vocab.here;
	compiler.leave_chain = 0;
	compiler.fuse_floor = vocab.here;

	DISPATCH(interp);
}

static void close_loop(Interpreter *interp) {
	for (int slot = compiler.leave_chain; slot != 0; ) {
		int prior_slot = (int)vocab.dict[slot];
		vocab.dict[slot] = vocab.here - slot;
		slot = prior_slot;
	}
	POP(leave_chain_val);
	POP(loop_begin_val);
	compiler.leave_chain = (int)VAL_NUMBER(leave_chain_val);
	compiler.loop_begin = (int)VAL_NUMBER(loop_begin_val);
}

void p_leave(DISPATCH_ARGS) {
	if (compiler.loop_begin == 0) {
		fail(interp, "leave: not inside a loop");
		return;
	}
	emit_call(interp, vocab.branch_cfa);
	emit(interp, (cell)compiler.leave_chain);
	compiler.leave_chain = vocab.here - 1;

	DISPATCH(interp);
}

void p_continue(DISPATCH_ARGS) {
	if (compiler.loop_begin == 0) {
		fail(interp, "continue: not inside a loop");
		return;
	}
	emit_call(interp, vocab.branch_cfa);
	emit(interp, compiler.loop_begin - vocab.here);

	DISPATCH(interp);
}

void p_until(DISPATCH_ARGS) {
	POP(back_val);
	int back = (int)VAL_NUMBER(back_val);
	if (!valid_patch_slot(interp, back, "until"))
		return;
	if (!try_fuse_cmp_branch(interp))
		emit_call(interp, vocab.zbranch_cfa);
	emit(interp, back - vocab.here);
	close_loop(interp);

	DISPATCH(interp);
}

void p_again(DISPATCH_ARGS) {
	POP(back_val);
	int back = (int)VAL_NUMBER(back_val);
	if (!valid_patch_slot(interp, back, "again"))
		return;
	emit_call(interp, vocab.branch_cfa);
	emit(interp, back - vocab.here);
	close_loop(interp);

	DISPATCH(interp);
}

void p_while(DISPATCH_ARGS) {
	if (!try_fuse_cmp_branch(interp))
		emit_call(interp, vocab.zbranch_cfa);
	push(interp, make_float((double)vocab.here));
	emit(interp, 0);

	DISPATCH(interp);
}

void p_repeat(DISPATCH_ARGS) {
	POP(exit_slot_val);
	POP(back_val);
	int exit_slot = (int)VAL_NUMBER(exit_slot_val);
	int back = (int)VAL_NUMBER(back_val);
	if (!valid_patch_slot(interp, back, "repeat") || !valid_patch_slot(interp, exit_slot, "repeat"))
		return;
	emit_call(interp, vocab.branch_cfa);
	emit(interp, back - vocab.here);
	vocab.dict[exit_slot] = (vocab.here - exit_slot);
	close_loop(interp);

	DISPATCH(interp);
}

static void open_quotation(Interpreter *interp) {
	int opener_start = compiler.input_buffer_pos - 2;
	int branch_slot = -1;
	if (compiler.compiling) {
		emit_call(interp, vocab.branch_cfa);
		branch_slot = vocab.here;
		emit(interp, 0);
	}
	int anon_cfa = vocab.here;
	emit(interp, (cell)&docol);
	compiler.fuse_floor = vocab.here;
	compiler.loadn_at = -1;
	enter_compile_scope(interp);
	compiler.compiling = 1;
	push(interp, make_float((double)anon_cfa));
	push(interp, make_float((double)branch_slot));
	push(interp, make_float((double)opener_start));
	push(interp, make_float((double)compiler.loop_begin));
	push(interp, make_float((double)compiler.leave_chain));
	compiler.loop_begin = 0;
	compiler.leave_chain = 0;
}

void p_qcolon(DISPATCH_ARGS) {
	open_quotation(interp);

	DISPATCH(interp);
}

static void record_quotation_span(int anon_cfa, int opener_start) {
	if (vocab.n_quotation_spans >= MAX_QUOTATION_SPANS)
		return;
	int snippet_len = compiler.input_buffer_pos - opener_start;
	int source_offset = 0;
	if (snippet_len > 0 && vocab.source_here + snippet_len + 1 <= SOURCE_POOL) {
		source_offset = vocab.source_here;
		memcpy(&vocab.source_pool[vocab.source_here],
				&compiler.input_buffer[opener_start], (size_t)snippet_len);
		vocab.source_pool[vocab.source_here + snippet_len] = 0;
		vocab.source_here += snippet_len + 1;
	}
	QuotationSpan *span = &vocab.quotation_spans[vocab.n_quotation_spans++];
	span->start_cfa = anon_cfa;
	span->end_cfa = vocab.here;
	span->source_offset = source_offset;
}

void truncate_quotation_spans(void) {
	while (vocab.n_quotation_spans > 0
			&& vocab.quotation_spans[vocab.n_quotation_spans - 1].end_cfa > vocab.here)
		vocab.n_quotation_spans--;
	for (int i = 0; i < vocab.n_quotation_spans; i++)
		if (vocab.quotation_spans[i].source_offset >= vocab.source_here)
			vocab.quotation_spans[i].source_offset = 0;
}

void p_qsemi(DISPATCH_ARGS) {
	if (compiler.loop_begin != 0) {
		fail(interp, ":] : unterminated loop (a begin has no until/again/repeat)");
		return;
	}
	leave_compile_scope(interp);
	emit_call(interp, vocab.exit_cfa);
	POP(leave_chain_val);
	POP(loop_begin_val);
	POP(opener_start_val);
	POP(branch_slot_val);
	POP(anon_cfa_val);
	compiler.leave_chain = (int)VAL_NUMBER(leave_chain_val);
	compiler.loop_begin = (int)VAL_NUMBER(loop_begin_val);
	int opener_start = (int)VAL_NUMBER(opener_start_val);
	int branch_slot = (int)VAL_NUMBER(branch_slot_val);
	int anon_cfa = (int)VAL_NUMBER(anon_cfa_val);
	record_quotation_span(anon_cfa, opener_start);
	if (branch_slot < 0) {
		compiler.compiling = 0;
		push(interp, make_xt(anon_cfa));
	} else {
		vocab.dict[branch_slot] = (vocab.here - branch_slot);
		emit_val_literal(interp, make_xt(anon_cfa));
	}

	DISPATCH(interp);
}

static int parse_word_cfa(Interpreter *interp, const char *op) {
	char *token = next_token();
	
	if (!token) {
		fail(interp, "%s : expected a word name", op);
		return 0;
	}
	int target_cfa = find(token);
	if (!target_cfa) {
		fail(interp, "%s : unknown word: %s", op, token);
		return 0;
	}

	return target_cfa;
}
	

void p_tick(DISPATCH_ARGS) {
	int target_cfa = parse_word_cfa(interp, "'");
	if (!target_cfa)
		return;

	Val value = make_xt(target_cfa);
	if (compiler.compiling)
		emit_val_literal(interp, value);
	else
		push(interp, value);

	DISPATCH(interp);
}

void p_lookup(DISPATCH_ARGS) {
	int target_cfa = parse_word_cfa(interp, "lookup");
	if (!target_cfa)
		return;

	push(interp, make_xt(target_cfa));

	DISPATCH(interp);
}
static void enter_compile_scope(Interpreter *interp) {
	if (compiler.n_local_scopes >= MAX_LOCAL_SCOPES) {
		fail(interp, "compile: locals nesting deeper than %d", MAX_LOCAL_SCOPES);
		return;
	}

	compiler.local_scope_starts[compiler.n_local_scopes] = compiler.n_local_names;
	compiler.local_scope_dict_starts[compiler.n_local_scopes] = vocab.here;
	compiler.n_local_scopes++;
}

static void leave_compile_scope(Interpreter *interp) {
	if (compiler.n_local_scopes <= 0)
		return;

	compiler.n_local_scopes--;
	int saved_n_names = compiler.local_scope_starts[compiler.n_local_scopes];
	int n_locals_in_scope = compiler.n_local_names - saved_n_names;

	if (n_locals_in_scope > 0) {
		emit_call(interp, vocab.leave_locals_cfa);
		emit(interp, (cell)n_locals_in_scope);
	}

	if (saved_n_names == 0) {
		compiler.local_names_pool_here = 0;
	} else {
		int last_offset = compiler.local_name_offsets[saved_n_names - 1];
		compiler.local_names_pool_here = last_offset +
			(int)strlen(&compiler.local_names_pool[last_offset]) + 1;
	}
	compiler.n_local_names = saved_n_names;
}

void p_colon(DISPATCH_ARGS) {
	char *token = next_token();
	if (!token) {
		fail(interp, ": expected a name for the new definition");
		return;
	}

	create_header(interp, token, 0);
	emit(interp, (cell)&docol);
	compiler.fuse_floor = vocab.here;
	compiler.loadn_at = -1;
	enter_compile_scope(interp);
	compiler.compiling = 1;
	compiler.loop_begin = 0;
	compiler.leave_chain = 0;

	compiler.compiling_src_start = compiler.input_buffer_pos;

	DISPATCH(interp);
}

int create_variable(Interpreter *interp, const char *name) {
	create_header(interp, name, 0);
	emit(interp, (cell)&dovar);
	emit(interp, (cell)make_float(0.0).bits);

	return vocab.latest_cfa;
}


void p_variable(DISPATCH_ARGS) {
	char *token = next_token();
	if (!token) {
		fail(interp, "variable: expected a name");
		return;
	}

	create_variable(interp, token);

	DISPATCH(interp);
}

void p_constant(DISPATCH_ARGS) {
	POP(value);
	char *token = next_token();
	if (!token) {
		fail(interp, "constant: expected a name");
		return;
	}
	create_header(interp, token, 2);
	emit(interp, (cell)&docol);
	emit_val_literal(interp, value);
	emit_call(interp, vocab.exit_cfa);
	DISPATCH(interp);
}

static void compile_locals_decl(Interpreter *interp, const char *opener, int force_all_receive) {
	if (!compiler.compiling || compiler.n_local_scopes <= 0) {
		fail(interp, "%s: only valid inside a colon definition or quotation", opener);
		return;
	}

	int scope_idx = compiler.n_local_scopes - 1;
	if (vocab.here != compiler.local_scope_dict_starts[scope_idx]) {
		fail(interp, "%s: locals must be declared at the head of the body", opener);
		return;
	}

	int scope_start = compiler.local_scope_starts[scope_idx];
	int receive_slots[MAX_LOCAL_NAMES];
	int n_received = 0;
	int lvar_slots[MAX_LOCAL_NAMES];
	int n_lvars = 0;

	while (1) {
		skip_whitespace_and_comments();
		char *token = next_token();
		if (!token) {
			if (refill_input())
				continue;
			fail(interp, "%s: unterminated locals declaration (no closing |)", opener);
			return;
		}
		if (strcmp(token, "|") == 0)
			break;

		int has_receive_marker = 0;
		if (token[0] == '>' && token[1] != 0) {
			has_receive_marker = 1;
			token++;
		}
		int has_lvar_marker = 0;
		if (token[0] == '?' && token[1] != 0) {
			has_lvar_marker = 1;
			token++;
		}
		if (has_lvar_marker && (has_receive_marker || (token[0] == '>' && token[1] != 0))) {
			fail(interp, "%s: '?' cannot combine with '>' (a received slot is not a fresh logic var)", opener);
			return;
		}

		for (int i = scope_start; i < compiler.n_local_names; i++) {
			if (strcmp(token, &compiler.local_names_pool[compiler.local_name_offsets[i]]) == 0) {
				fail(interp, "%s: local '%s' declared twice", opener, token);
				return;
			}
		}

		int name_len = (int)strlen(token);
		if (compiler.local_names_pool_here + name_len + 1 > LOCAL_NAMES_POOL_SIZE) {
			fail(interp, "%s: local names pool full", opener);
			return;
		}
		if (compiler.n_local_names >= MAX_LOCAL_NAMES) {
			fail(interp, "%s: too many local names (max %d)", opener, MAX_LOCAL_NAMES);
			return;
		}

		int slot = compiler.n_local_names - scope_start;

		int offset = compiler.local_names_pool_here;
		memcpy(&compiler.local_names_pool[offset], token, (size_t)name_len);
		compiler.local_names_pool[offset + name_len] = 0;
		compiler.local_names_pool_here += name_len + 1;
		compiler.local_name_offsets[compiler.n_local_names++] = offset;

		if (has_receive_marker)
			receive_slots[n_received++] = slot;
		if (has_lvar_marker)
			lvar_slots[n_lvars++] = slot;
	}

	int n_declared = compiler.n_local_names - scope_start;
	if (n_declared == 0)
		return;

	if (force_all_receive && n_received > 0) {
		fail(interp, "%s: do not use > markers; %s already implies all-receive", opener, opener);
		return;
	}
	if (force_all_receive && n_lvars > 0) {
		fail(interp, "%s: no '?' markers; %s receives every slot, so none can hold a fresh logic var", opener, opener);
		return;
	}

	if (force_all_receive || n_received == n_declared) {
		emit_call(interp, vocab.enter_locals_to_cfa);
		emit(interp, (cell)n_declared);
	} else if (n_received == 0) {
		emit_call(interp, vocab.enter_locals_cfa);
		emit(interp, (cell)n_declared);
	} else {
		emit_call(interp, vocab.enter_locals_mixed_cfa);
		emit(interp, (cell)n_declared);
		emit(interp, (cell)n_received);
		for (int i = 0; i < n_received; i++)
			emit(interp, (cell)receive_slots[i]);
	}

	if (n_lvars > 0) {
		int lvar_cfa = find("lvar");
		for (int i = 0; i < n_lvars; i++) {
			emit_call(interp, lvar_cfa);
			emit_call(interp, vocab.local_store_0depth_cfa);
			emit(interp, (cell)lvar_slots[i]);
		}
	}
}

void p_bar(DISPATCH_ARGS) {
	compile_locals_decl(interp, "|", 0);

	DISPATCH(interp);
}

void p_bar_to(DISPATCH_ARGS) {
	compile_locals_decl(interp, "|>", 1);

	DISPATCH(interp);
}

void p_bracket_bar(DISPATCH_ARGS) {
	open_quotation(interp);
	compile_locals_decl(interp, "[|", 0);

	DISPATCH(interp);
}

void p_bracket_bar_to(DISPATCH_ARGS) {
	open_quotation(interp);
	compile_locals_decl(interp, "[>", 1);

	DISPATCH(interp);
}

void p_to_var(DISPATCH_ARGS) {
	int var_cfa = (int)vocab.dict[interp->ip++];
	POP(value);
	vocab.dict[var_cfa + 1] = (cell)value.bits;

	DISPATCH(interp);
}

void p_to(DISPATCH_ARGS) {
	char *token = next_token();
	if (!token) {
		fail(interp, "to: expected a name"); 
		return;
	}

	if (compiler.compiling) {
		int local_depth, local_slot_idx;
		if (find_local(token, &local_depth, &local_slot_idx)) {
			if (try_fuse_local_acc(interp, local_depth, local_slot_idx))
				return;
			if (local_depth == 0) {
				emit_call(interp, vocab.local_store_0depth_cfa);
				emit(interp, (cell)local_slot_idx);
			} else {
				emit_call(interp, vocab.local_store_cfa);
				emit(interp, (cell)local_depth);
				emit(interp, (cell)local_slot_idx);
			}
			return;
		}
	}

	int target_cfa = find(token);
	if (!target_cfa) {
		if (compiler.compiling) {
			fail(interp, "to: unknown variable: %s; declare it with variable", token); 
		return; 
		}
		target_cfa = create_variable(interp, token);
	}

	cfa_handler h = (cfa_handler)vocab.dict[target_cfa];
	if (h != dovar) {
		fail(interp, "to: %s is not a variable", token); 
		return; 
	}

	if (compiler.compiling) {
		if (!superword_try_fuse_store(interp, target_cfa)) {
			emit_call(interp, vocab.to_var_cfa);
			emit(interp, (cell)target_cfa);
		}
	} else {
		POP(value);
		vocab.dict[target_cfa + 1] = (cell)value.bits;
	}

	DISPATCH(interp);
}

static void compile_local_unary(Interpreter *interp, const char *op,
                                int depth0_cfa, int fallback_cfa) {
	char *token = next_token();
	if (!token) {
		fail(interp, "%s: expected a local name", op);
		return;
	}
	if (!compiler.compiling) {
		fail(interp, "%s: only valid inside a colon definition", op);
		return;
	}
	int depth, slot;
	if (!find_local(token, &depth, &slot)) {
		fail(interp, "%s: %s is not a local", op, token);
		return;
	}
	if (depth == 0) {
		emit_call(interp, depth0_cfa);
		emit(interp, (cell)slot);
	} else {
		emit_call(interp, vocab.local_fetch_cfa);
		emit(interp, (cell)depth);
		emit(interp, (cell)slot);
		emit_call(interp, fallback_cfa);
		emit_call(interp, vocab.local_store_cfa);
		emit(interp, (cell)depth);
		emit(interp, (cell)slot);
	}
}

void p_increment(DISPATCH_ARGS) {
	compile_local_unary(interp, "++",
	                    vocab.local_incr_0depth_cfa,
	                    vocab.inc_cfa);

	DISPATCH(interp);
}

void p_decrement(DISPATCH_ARGS) {
	compile_local_unary(interp, "--",
	                    vocab.local_decr_0depth_cfa,
	                    vocab.dec_cfa);

	DISPATCH(interp);
}

void p_f_increment(DISPATCH_ARGS) {
	compile_local_unary(interp, "f++",
	                    vocab.local_finc_0depth_cfa,
	                    vocab.finc_cfa);

	DISPATCH(interp);
}

void p_f_decrement(DISPATCH_ARGS) {
	compile_local_unary(interp, "f--",
	                    vocab.local_fdec_0depth_cfa,
	                    vocab.fdec_cfa);

	DISPATCH(interp);
}

void p_inline(DISPATCH_ARGS) {
	int latest = vocab.latest_cfa;
	if (!latest) {
		fail(interp, "inline: no recent definition");
		return;
	}

	WORD_FLAGS(latest) |= 2;

	DISPATCH(interp);
}

void p_symbol(DISPATCH_ARGS) {
	char *token = next_token();
	if (!token) {
		fail(interp, "symbol: expected a name");
		return;
	}
	if (token[0] == ':')
		token++;

	create_header(interp, token, 0);
	emit(interp, (cell)&dosym);

	emit(interp, (cell)intern_symbol(interp, token));

	DISPATCH(interp);
}

void p_string_to_symbol(DISPATCH_ARGS) {
	POP_STRING(string, "string>symbol");
	push(interp, make_symbol(intern_symbol(interp, string->bytes)));

	DISPATCH(interp);
}

void p_internal(DISPATCH_ARGS) {
	if (vocab.latest_cfa == 0) {
		fail(interp, "internal: no definition to mark");
		return;
	}
	WORD_FLAGS(vocab.latest_cfa) |= 4;

	DISPATCH(interp);
}

void p_forget(DISPATCH_ARGS) {
	char *token = next_token();
	if (!token) {
		fail(interp, "forget: expected a name");
		return;
	}
	int target_cfa = find(token);
	if (!target_cfa) {
		fail(interp, "forget: unknown word: %s", token);
		return;
	}
	vocab.here = target_cfa - 4;
	vocab.forget_generation++;
	vocab.names_here = (int)WORD_NAME(target_cfa);
	vocab.latest_cfa = (int)WORD_LINK(target_cfa);

	int max_src_end = 1;
	for (int surviving_cfa = vocab.latest_cfa; surviving_cfa != 0; surviving_cfa = (int)WORD_LINK(surviving_cfa)) {
		int src_offset = (int)WORD_SOURCE(surviving_cfa);
		if (src_offset > 0) {
			int src_end = src_offset + (int)strlen(&vocab.source_pool[src_offset]) + 1;
			max_src_end = MAX(max_src_end, src_end);
		}
	}
	vocab.source_here = max_src_end;
	truncate_quotation_spans();

	DISPATCH(interp);
}
