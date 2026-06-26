#include "logicforth.h"

#define FLOAT_BINOPS(X) \
	X(add, +, p_add_f) \
	X(sub, -, p_sub_f) \
	X(mul, *, p_mul_f) \
	X(div, /, p_div_f)

#define FLOAT_UNARY_FNS(X) \
	X(sq,   v * v,   p_sq) \
	X(neg,  -v,      p_neg) \
	X(abs,  fabs(v), p_abs) \
	X(sqrt, sqrt(v), p_sqrt) \
	X(exp,  exp(v),  p_exp) \
	X(log,  log10(v),  p_log) \
	X(sin,  sin(v),  p_sin) \
	X(cos,  cos(v),  p_cos) \
	X(tan,  tan(v),  p_tan) \
	X(tanh, tanh(v), p_tanh)

#define FLOAT_FUSED(X) \
	X(fma, a->number * b.number + c.number, p_fmul_add, "vvf*+") \
	X(fms, c.number - a->number * b.number, p_fmul_sub, "vvf*-")

static int next_var_slot(Interpreter *interp, const char *op) {
	char *token = next_token();
	if (!token) {
		fail(interp, "%s: expected a variable name", op);
		return -1;
	}
	int var_cfa = find(token);
	if (!var_cfa || (cfa_handler)vocab.dict[var_cfa] != dovar) {
		fail(interp, "%s: %s is not a variable", op, token);
		return -1;
	}
	return var_cfa + 1;
}

static void compile_two_var_op(Interpreter *interp, int runtime_cfa, const char *op) {
	if (!compiler.compiling) {
		fail(interp, "%s: only valid while compiling", op);
		return;
	}
	int slot1 = next_var_slot(interp, op);
	if (slot1 < 0) {
		return;
	}
	int slot2 = next_var_slot(interp, op);
	if (slot2 < 0) {
		return;
	}
	emit_call(interp, runtime_cfa);
	emit(interp, (cell)slot1);
	emit(interp, (cell)slot2);
}

static void compile_one_var_op(Interpreter *interp, int runtime_cfa, const char *op) {
	if (!compiler.compiling) {
		fail(interp, "%s: only valid while compiling", op);
		return;
	}
	int slot = next_var_slot(interp, op);
	if (slot < 0) {
		return;
	}
	emit_call(interp, runtime_cfa);
	emit(interp, (cell)slot);
}

static void p_fmul_add(Interpreter *interp) {
	if (interp->dsp < 3) {
		fail(interp, "f*+: data stack underflow");
		return;
	}
	Val *a = &interp->data_stack[interp->dsp - 3];
	Val *b = &interp->data_stack[interp->dsp - 2];
	Val *c = &interp->data_stack[interp->dsp - 1];
	a->number = a->number * b->number + c->number;
	interp->dsp -= 2;

	DISPATCH(interp);
}

static void p_fmul_sub(Interpreter *interp) {
	if (interp->dsp < 3) {
		fail(interp, "f*-: data stack underflow");
		return;
	}
	Val *a = &interp->data_stack[interp->dsp - 3];
	Val *b = &interp->data_stack[interp->dsp - 2];
	Val *c = &interp->data_stack[interp->dsp - 1];
	a->number = c->number - a->number * b->number;
	interp->dsp -= 2;

	DISPATCH(interp);
}

#define GEN_VV(suffix, op, base) \
	static int vv_##suffix##_cfa; \
	static void p_vv_##suffix(Interpreter *interp) { \
		cell s1 = vocab.dict[interp->ip++]; \
		cell s2 = vocab.dict[interp->ip++]; \
		Val a, b; \
		a.bits = (uint64_t)vocab.dict[s1]; \
		b.bits = (uint64_t)vocab.dict[s2]; \
		push(interp, make_float(a.number op b.number)); \
		DISPATCH(interp); \
	} \
	static void p_compile_vv_##suffix(Interpreter *interp) { \
		compile_two_var_op(interp, vv_##suffix##_cfa, "vvf" #op); \
	}
FLOAT_BINOPS(GEN_VV)

#define GEN_VV_STORE(suffix, op, base) \
	static int vv_##suffix##_store_cfa; \
	static void p_vv_##suffix##_store(Interpreter *interp) { \
		cell s1 = vocab.dict[interp->ip++]; \
		cell s2 = vocab.dict[interp->ip++]; \
		cell dst = vocab.dict[interp->ip++]; \
		Val a, b; \
		a.bits = (uint64_t)vocab.dict[s1]; \
		b.bits = (uint64_t)vocab.dict[s2]; \
		vocab.dict[dst] = (cell)make_float(a.number op b.number).bits; \
		DISPATCH(interp); \
	}
FLOAT_BINOPS(GEN_VV_STORE)

#define GEN_VF(suffix, op, base) \
	static int vf_##suffix##_cfa; \
	static void p_vf_##suffix(Interpreter *interp) { \
		cell s = vocab.dict[interp->ip++]; \
		if (interp->dsp < 1) { \
			fail(interp, "vf" #op ": data stack underflow"); \
			return; \
		} \
		Val x; \
		x.bits = (uint64_t)vocab.dict[s]; \
		Val *a = &interp->data_stack[interp->dsp - 1]; \
		a->number = a->number op x.number; \
		DISPATCH(interp); \
	} \
	static void p_compile_vf_##suffix(Interpreter *interp) { \
		compile_one_var_op(interp, vf_##suffix##_cfa, "vf" #op); \
	}
FLOAT_BINOPS(GEN_VF)

#define GEN_LF(suffix, op, base) \
	static int lf_##suffix##_cfa; \
	static void p_lf_##suffix(Interpreter *interp) { \
		Val k; \
		k.bits = (uint64_t)vocab.dict[interp->ip++]; \
		if (interp->dsp < 1) { \
			fail(interp, "lf" #op ": data stack underflow"); \
			return; \
		} \
		Val *a = &interp->data_stack[interp->dsp - 1]; \
		a->number = a->number op k.number; \
		DISPATCH(interp); \
	}
FLOAT_BINOPS(GEN_LF)

#define GEN_VFN(suffix, expr, base) \
	static int vfn_##suffix##_cfa; \
	static void p_vfn_##suffix(Interpreter *interp) { \
		cell s = vocab.dict[interp->ip++]; \
		Val cell_v; \
		cell_v.bits = (uint64_t)vocab.dict[s]; \
		double v = cell_v.number; \
		push(interp, make_float(expr)); \
		DISPATCH(interp); \
	} \
	static void p_compile_vfn_##suffix(Interpreter *interp) { \
		compile_one_var_op(interp, vfn_##suffix##_cfa, "vf" #suffix); \
	}
FLOAT_UNARY_FNS(GEN_VFN)

#define GEN_FUSED(suffix, expr, base, name) \
	static int vv_##suffix##_cfa; \
	static void p_vv_##suffix(Interpreter *interp) { \
		cell s1 = vocab.dict[interp->ip++]; \
		cell s2 = vocab.dict[interp->ip++]; \
		if (interp->dsp < 1) { \
			fail(interp, name ": data stack underflow"); \
			return; \
		} \
		Val b, c; \
		b.bits = (uint64_t)vocab.dict[s1]; \
		c.bits = (uint64_t)vocab.dict[s2]; \
		Val *a = &interp->data_stack[interp->dsp - 1]; \
		a->number = expr; \
		DISPATCH(interp); \
	} \
	static void p_compile_vv_##suffix(Interpreter *interp) { \
		compile_two_var_op(interp, vv_##suffix##_cfa, name); \
	}
FLOAT_FUSED(GEN_FUSED)

#define GEN_FUSED_STORE(suffix, expr, base, name) \
	static int vv_##suffix##_store_cfa; \
	static void p_vv_##suffix##_store(Interpreter *interp) { \
		cell s1 = vocab.dict[interp->ip++]; \
		cell s2 = vocab.dict[interp->ip++]; \
		cell dst = vocab.dict[interp->ip++]; \
		if (interp->dsp < 1) { \
			fail(interp, name "!: data stack underflow"); \
			return; \
		} \
		Val b, c; \
		b.bits = (uint64_t)vocab.dict[s1]; \
		c.bits = (uint64_t)vocab.dict[s2]; \
		Val *a = &interp->data_stack[--interp->dsp]; \
		vocab.dict[dst] = (cell)make_float(expr).bits; \
		DISPATCH(interp); \
	}
FLOAT_FUSED(GEN_FUSED_STORE)

#define GEN_VF_STORE(suffix, op, base) \
	static int vf_##suffix##_store_cfa; \
	static void p_vf_##suffix##_store(Interpreter *interp) { \
		cell s = vocab.dict[interp->ip++]; \
		cell dst = vocab.dict[interp->ip++]; \
		if (interp->dsp < 1) { \
			fail(interp, "vf" #op "!: data stack underflow"); \
			return; \
		} \
		Val x; \
		x.bits = (uint64_t)vocab.dict[s]; \
		double a = interp->data_stack[--interp->dsp].number; \
		vocab.dict[dst] = (cell)make_float(a op x.number).bits; \
		DISPATCH(interp); \
	}
FLOAT_BINOPS(GEN_VF_STORE)

#define GEN_VFN_STORE(suffix, expr, base) \
	static int vfn_##suffix##_store_cfa; \
	static void p_vfn_##suffix##_store(Interpreter *interp) { \
		cell s = vocab.dict[interp->ip++]; \
		cell dst = vocab.dict[interp->ip++]; \
		Val cell_v; \
		cell_v.bits = (uint64_t)vocab.dict[s]; \
		double v = cell_v.number; \
		vocab.dict[dst] = (cell)make_float(expr).bits; \
		DISPATCH(interp); \
	}
FLOAT_UNARY_FNS(GEN_VFN_STORE)

static int emit_fused_two_var(Interpreter *interp, int runtime_cfa, int slot_a, int slot_b) {
	vocab.here -= 4;
	emit_call(interp, runtime_cfa);
	emit(interp, (cell)slot_a);
	emit(interp, (cell)slot_b);
	compiler.fuse_prev_var = 0;
	compiler.fuse_prev2_var = 0;
	return 1;
}

static int emit_fused_one_var(Interpreter *interp, int runtime_cfa, int slot) {
	vocab.here -= 2;
	emit_call(interp, runtime_cfa);
	emit(interp, (cell)slot);
	compiler.fuse_prev_var = 0;
	compiler.fuse_prev2_var = 0;
	return 1;
}

int superword_cell_count(cell handler) {
	cfa_handler h = (cfa_handler)handler;
#define CC_VV(suffix, op, base) if (h == p_vv_##suffix) return 3;
	FLOAT_BINOPS(CC_VV)
#define CC_VV_STORE(suffix, op, base) if (h == p_vv_##suffix##_store) return 4;
	FLOAT_BINOPS(CC_VV_STORE)
#define CC_FUSED_STORE(suffix, expr, base, name) if (h == p_vv_##suffix##_store) return 4;
	FLOAT_FUSED(CC_FUSED_STORE)
#define CC_VF_STORE(suffix, op, base) if (h == p_vf_##suffix##_store) return 3;
	FLOAT_BINOPS(CC_VF_STORE)
#define CC_VFN_STORE(suffix, expr, base) if (h == p_vfn_##suffix##_store) return 3;
	FLOAT_UNARY_FNS(CC_VFN_STORE)
#define CC_VF(suffix, op, base) if (h == p_vf_##suffix) return 2;
	FLOAT_BINOPS(CC_VF)
#define CC_LF(suffix, op, base) if (h == p_lf_##suffix) return 2;
	FLOAT_BINOPS(CC_LF)
#define CC_VFN(suffix, expr, base) if (h == p_vfn_##suffix) return 2;
	FLOAT_UNARY_FNS(CC_VFN)
#define CC_FUSED(suffix, expr, base, name) if (h == p_vv_##suffix) return 3;
	FLOAT_FUSED(CC_FUSED)
	return 0;
}

static int store_i_drop_cfa;
static int inc_store_i_cfa;
static int dec_store_i_cfa;
static int add_store_i_cfa;
static int sub_store_i_cfa;
static int mul_store_i_cfa;
static int div_store_i_cfa;

/* Fuse the array read-modify-write idiom
 *   <arr> <idx> <arr> <idx> @i [<delta>] <op> !i drop
 * into  <arr> <idx> [<delta>] (<op>!i)  — dropping the redundant re-fetch.
 * Called when compiling the trailing `drop` (with `!i` already at here-1). The
 * index is always a depth-0 local; the array push and fetch form must agree:
 *   @i.l0   -> array is a (dovar) var pushed separately
 *   @i.ll0  -> array is a depth-0 local (folded into the fetch op)
 *   @i.l1l0 -> array is a depth-1 local (folded into the fetch op)
 * <op> is unary f1+ or f1- (delta = +-1, no delta cells) or a binary
 * f-add/f-sub/f-mul/f-div with a single 2-cell delta push (literal, local,
 * or var). */
static int try_fuse_array_step(Interpreter *interp) {
	cell *dict = vocab.dict;
	int here = vocab.here;
	int floor = compiler.fuse_floor;

	if (here - 2 < floor)
		return 0;
	cfa_handler arith = (cfa_handler)dict[here - 2];
	int op_cfa;
	int has_delta;
	int fetch_end;
	if (arith == p_inc) {
		op_cfa = inc_store_i_cfa; has_delta = 0; fetch_end = here - 2;
	} else if (arith == p_dec) {
		op_cfa = dec_store_i_cfa; has_delta = 0; fetch_end = here - 2;
	} else if (arith == p_add_f) {
		op_cfa = add_store_i_cfa; has_delta = 1; fetch_end = here - 4;
	} else if (arith == p_sub_f) {
		op_cfa = sub_store_i_cfa; has_delta = 1; fetch_end = here - 4;
	} else if (arith == p_mul_f) {
		op_cfa = mul_store_i_cfa; has_delta = 1; fetch_end = here - 4;
	} else if (arith == p_div_f) {
		op_cfa = div_store_i_cfa; has_delta = 1; fetch_end = here - 4;
	} else {
		return 0;
	}

	if (has_delta) {
		if (here - 4 < floor)
			return 0;
		cfa_handler push = (cfa_handler)dict[here - 4];
		if (push != p_literal && push != p_local_fetch_0depth
		    && push != p_local_fetch_1depth && push != dovar)
			return 0;
	}

	if (fetch_end - 3 < floor)
		return 0;
	cfa_handler folded = (cfa_handler)dict[fetch_end - 3];
	int arr_form;
	int arr_key;
	int idx_slot;
	int fetch_start;
	if (folded == p_at_i_ll0 || folded == p_at_i_l1l0) {
		arr_form = (folded == p_at_i_ll0) ? 1 : 2;
		arr_key = (int)dict[fetch_end - 2];
		idx_slot = (int)dict[fetch_end - 1];
		fetch_start = fetch_end - 3;
	} else {
		if (fetch_end - 4 < floor)
			return 0;
		if ((cfa_handler)dict[fetch_end - 2] != p_at_i_local0)
			return 0;
		if ((cfa_handler)dict[fetch_end - 4] != dovar)
			return 0;
		arr_form = 0;
		arr_key = (int)dict[fetch_end - 3];
		idx_slot = (int)dict[fetch_end - 1];
		fetch_start = fetch_end - 4;
	}

	if (fetch_start - 4 < floor)
		return 0;
	if ((cfa_handler)dict[fetch_start - 2] != p_local_fetch_0depth)
		return 0;
	if ((int)dict[fetch_start - 1] != idx_slot)
		return 0;
	cfa_handler arr_push = (cfa_handler)dict[fetch_start - 4];
	if (arr_form == 0 && arr_push != dovar)
		return 0;
	if (arr_form == 1 && arr_push != p_local_fetch_0depth)
		return 0;
	if (arr_form == 2 && arr_push != p_local_fetch_1depth)
		return 0;
	if ((int)dict[fetch_start - 3] != arr_key)
		return 0;

	if (has_delta) {
		dict[fetch_start] = dict[fetch_end];
		dict[fetch_start + 1] = dict[fetch_end + 1];
		vocab.here = fetch_start + 2;
	} else {
		vocab.here = fetch_start;
	}
	emit_call(interp, op_cfa);
	return 1;
}

int superword_is_lit_fold(cell handler) {
	cfa_handler h = (cfa_handler)handler;
#define IS_LF(suffix, op, base) if (h == p_lf_##suffix) return 1;
	FLOAT_BINOPS(IS_LF)
	return 0;
}

int superword_try_fuse(Interpreter *interp, int op_cfa) {
	if (op_cfa == vocab.at_i_cfa) {
		if (try_fuse_at_i_ll(interp))
			return 1;
		if (try_fuse_at_i_local(interp))
			return 1;
		if (try_fuse_gather_local(interp))
			return 1;
		return try_fuse_at_i_lit(interp);
	}
	cfa_handler op_handler = (cfa_handler)vocab.dict[op_cfa];

	if (op_handler == p_drop
	    && vocab.here >= 1 && vocab.here - 1 >= compiler.fuse_floor
	    && (cfa_handler)vocab.dict[vocab.here - 1] == p_store_i) {
		if (try_fuse_array_step(interp))
			return 1;
		vocab.here -= 1;
		emit_call(interp, store_i_drop_cfa);
		return 1;
	}

	if (try_fuse_local_arith(interp, op_handler))
		return 1;
	int prev1 = compiler.fuse_prev_var;
	int prev2 = compiler.fuse_prev2_var;

#define FUSE_BIN(suffix, op, base) \
	if (op_handler == base) { \
		if (vocab.here >= 2 && vocab.here - 2 >= compiler.fuse_floor \
		    && (cfa_handler)vocab.dict[vocab.here - 2] == p_literal) { \
			cell bits = vocab.dict[vocab.here - 1]; \
			vocab.here -= 2; \
			emit_call(interp, lf_##suffix##_cfa); \
			emit(interp, bits); \
			compiler.fuse_prev_var = 0; \
			compiler.fuse_prev2_var = 0; \
			return 1; \
		} \
		if (prev1 && prev2) return emit_fused_two_var(interp, vv_##suffix##_cfa, prev2 + 1, prev1 + 1); \
		if (prev1) return emit_fused_one_var(interp, vf_##suffix##_cfa, prev1 + 1); \
		return 0; \
	}
	FLOAT_BINOPS(FUSE_BIN)

#define FUSE_FN(suffix, expr, base) \
	if (op_handler == base) { \
		if (prev1) return emit_fused_one_var(interp, vfn_##suffix##_cfa, prev1 + 1); \
		return 0; \
	}
	FLOAT_UNARY_FNS(FUSE_FN)

#define FUSE_FUSED(suffix, expr, base, name) \
	if (op_handler == base) { \
		if (prev1 && prev2) return emit_fused_two_var(interp, vv_##suffix##_cfa, prev2 + 1, prev1 + 1); \
		return 0; \
	}
	FLOAT_FUSED(FUSE_FUSED)

	return 0;
}

int superword_try_fuse_store(Interpreter *interp, int dst_cfa) {
	if (!compiler.compiling)
		return 0;
	cell *dict = vocab.dict;
	int here = vocab.here;
	int dst = dst_cfa + 1;

	if (here >= 3) {
		cfa_handler tail = (cfa_handler)dict[here - 3];
		int store_cfa = -1;
#define MATCH3(suffix, ...) if (tail == p_vv_##suffix) store_cfa = vv_##suffix##_store_cfa;
		FLOAT_BINOPS(MATCH3)
		FLOAT_FUSED(MATCH3)
		if (store_cfa >= 0) {
			cell s1 = dict[here - 2];
			cell s2 = dict[here - 1];
			vocab.here -= 3;
			emit_call(interp, store_cfa);
			emit(interp, s1);
			emit(interp, s2);
			emit(interp, (cell)dst);
			return 1;
		}
	}

	if (here >= 2) {
		cfa_handler tail = (cfa_handler)dict[here - 2];
		int store_cfa = -1;
#define MATCH2_VF(suffix, op, base) if (tail == p_vf_##suffix) store_cfa = vf_##suffix##_store_cfa;
		FLOAT_BINOPS(MATCH2_VF)
#define MATCH2_VFN(suffix, expr, base) if (tail == p_vfn_##suffix) store_cfa = vfn_##suffix##_store_cfa;
		FLOAT_UNARY_FNS(MATCH2_VFN)
		if (store_cfa >= 0) {
			cell s = dict[here - 1];
			vocab.here -= 2;
			emit_call(interp, store_cfa);
			emit(interp, s);
			emit(interp, (cell)dst);
			return 1;
		}
	}

	return 0;
}

void define_superwords(Interpreter *interp) {
	define_primitive(interp, "f*+", p_fmul_add, 0);
	define_primitive(interp, "f*-", p_fmul_sub, 0);
	store_i_drop_cfa = define_primitive(interp, "(!i-drop)", p_store_i_drop, 0);
	inc_store_i_cfa = define_primitive(interp, "(inc!i)", p_inc_store_i, 4);
	dec_store_i_cfa = define_primitive(interp, "(dec!i)", p_dec_store_i, 4);
	add_store_i_cfa = define_primitive(interp, "(+!i)", p_add_store_i, 4);
	sub_store_i_cfa = define_primitive(interp, "(-!i)", p_sub_store_i, 4);
	mul_store_i_cfa = define_primitive(interp, "(*!i)", p_mul_store_i, 4);
	div_store_i_cfa = define_primitive(interp, "(/!i)", p_div_store_i, 4);

#define REG_LF(suffix, op, base) \
	lf_##suffix##_cfa = define_primitive(interp, "(lf" #op ")", p_lf_##suffix, 4);
	FLOAT_BINOPS(REG_LF)

#define REG_VV(suffix, op, base) \
	vv_##suffix##_cfa = define_primitive(interp, "(vvf" #op ")", p_vv_##suffix, 4); \
	define_primitive(interp, "vvf" #op, p_compile_vv_##suffix, 1);
	FLOAT_BINOPS(REG_VV)

#define REG_VV_STORE(suffix, op, base) \
	vv_##suffix##_store_cfa = define_primitive(interp, "(vvf" #op "!)", p_vv_##suffix##_store, 4);
	FLOAT_BINOPS(REG_VV_STORE)

#define REG_FUSED_STORE(suffix, expr, base, name) \
	vv_##suffix##_store_cfa = define_primitive(interp, "(" name "!)", p_vv_##suffix##_store, 4);
	FLOAT_FUSED(REG_FUSED_STORE)

#define REG_VF_STORE(suffix, op, base) \
	vf_##suffix##_store_cfa = define_primitive(interp, "(vf" #op "!)", p_vf_##suffix##_store, 4);
	FLOAT_BINOPS(REG_VF_STORE)

#define REG_VFN_STORE(suffix, expr, base) \
	vfn_##suffix##_store_cfa = define_primitive(interp, "(vf" #suffix "!)", p_vfn_##suffix##_store, 4);
	FLOAT_UNARY_FNS(REG_VFN_STORE)

#define REG_VF(suffix, op, base) \
	vf_##suffix##_cfa = define_primitive(interp, "(vf" #op ")", p_vf_##suffix, 4); \
	define_primitive(interp, "vf" #op, p_compile_vf_##suffix, 1);
	FLOAT_BINOPS(REG_VF)

#define REG_VFN(suffix, expr, base) \
	vfn_##suffix##_cfa = define_primitive(interp, "(vf" #suffix ")", p_vfn_##suffix, 4); \
	define_primitive(interp, "vf" #suffix, p_compile_vfn_##suffix, 1);
	FLOAT_UNARY_FNS(REG_VFN)

#define REG_FUSED(suffix, expr, base, name) \
	vv_##suffix##_cfa = define_primitive(interp, "(" name ")", p_vv_##suffix, 4); \
	define_primitive(interp, name, p_compile_vv_##suffix, 1);
	FLOAT_FUSED(REG_FUSED)
}
