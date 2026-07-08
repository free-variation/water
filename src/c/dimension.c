#include "water.h"

typedef struct {
	int numerator;
	int denominator;
} Rational;

typedef struct {
	int dimension;
	Rational power;
} DimTerm;

typedef struct {
	DimTerm *terms;
	int n_terms;
	Rational scale;
	int name;
} Unit;

int *dimension_names;
int n_dimensions;
int dimension_cap;
Unit *units;
int n_units;
int unit_cap;
int init_n_dimensions;
int init_n_units;

#define DIMENSION_UNNAMED (-1)
#define UNIT_UNNAMED (-1)

void dimension_init() {
	unit_cap = 8;
	units = malloc(sizeof(Unit) * (size_t)unit_cap);
	
	units[0].terms = NULL;
	units[0].n_terms = 0;
	units[0].scale.numerator = 1;
	units[0].scale.denominator = 1;
	units[0].name = UNIT_UNNAMED;
	n_units = 1;

	dimension_names = NULL;
	n_dimensions = 0;
	dimension_cap = 0;
}

void dimension_freeze() {
	init_n_dimensions = n_dimensions;
	init_n_units = n_units;
}

static int gcd(int a, int b) {
	a = a < 0 ? -a : a;
	b = b < 0 ? -b : b;

	while (b) {
		int remainder = a % b;
		a = b;
		b = remainder;
	}

	return a ? a : 1;
}

static long long gcd_ll(long long a, long long b) {
	a = a < 0 ? -a : a;
	b = b < 0 ? -b : b;

	while (b) {
		long long remainder = a % b;
		a = b;
		b = remainder;
	}

	return a ? a : 1;
}

static int exact_root(int radicand, int degree, int *root) {
	for (int candidate = 1; ; candidate++) {
		long long power = 1;
		for (int i = 0; i < degree && power <= radicand; i++)
			power *= candidate;

		if (power == radicand) {
			*root = candidate;
			return 1;
		}

		if (power > radicand) 
			return 0;
	}
}

static Rational make_rational(int numerator, int denominator) {
	if (denominator < 0) {
		numerator = -numerator;
		denominator = -denominator;
	}

	int divisor = gcd(numerator, denominator);

	Rational reduced;
	reduced.numerator = numerator / divisor;
	reduced.denominator = denominator / divisor;

	return reduced;
}

static int rational_from_ll(long long numerator, long long denominator, Rational *reduced) {
	if (denominator < 0) {
		numerator = -numerator;
		denominator = -denominator;
	}

	long long divisor = gcd_ll(numerator, denominator);
	numerator /= divisor;
	denominator /= divisor;

	if (numerator < INT_MIN || numerator > INT_MAX || denominator > INT_MAX)
		return 0;

	reduced->numerator = (int)numerator;
	reduced->denominator = (int)denominator;
	return 1;
}

static int rational_add(Rational left, Rational right, Rational *sum) {
	return rational_from_ll(
			(long long)left.numerator * right.denominator + (long long)right.numerator * left.denominator,
			(long long)left.denominator * right.denominator, sum);
}

static int rational_multiply(Rational left, Rational right, Rational *product) {
	return rational_from_ll(
			(long long)left.numerator * right.numerator,
			(long long)left.denominator * right.denominator, product);
}

static int rational_divide(Rational left, Rational right, Rational *quotient) {
	return rational_from_ll(
			(long long)left.numerator * right.denominator,
			(long long)left.denominator * right.numerator, quotient);
}

static Rational rational_pow(Rational scale, Rational exponent, int *success) {
	*success = 1;

	int power = exponent.numerator;
	int root_degree = exponent.denominator;

	if (power < 0) {
		scale = make_rational(scale.denominator, scale.numerator);
		power = -power;
	}

	int numerator = scale.numerator;
	int denominator = scale.denominator;

	if (root_degree > 1) {
		if (numerator == 1 && denominator == 1)
				return make_rational(1, 1);

		if (!exact_root(numerator, root_degree, &numerator)
				|| !exact_root(denominator, root_degree, &denominator)) {
			*success = 0;
			return make_rational(1, 1);
		}
	}

	long long raised_numerator = 1;
	long long raised_denominator = 1;

	for (int i = 0; i < power; i++) {
		raised_numerator *= numerator;
		raised_denominator *= denominator;

		if (raised_numerator > INT_MAX || raised_denominator > INT_MAX) {
			*success = 0;
			return make_rational(1, 1);
		}
	}

	return make_rational((int)raised_numerator, (int)raised_denominator);
}


static int rational_of_double(double value, Rational *rational) {
	for (int denominator = 1; denominator <= 1000000; denominator++) {
		double scaled = value * denominator;
		double rounded = round(scaled);
		if (rounded >= 1 && rounded <= INT_MAX && fabs(scaled - rounded) < 1e-9) {
			*rational = make_rational((int)rounded, denominator);
			return 1;
		}
	}
	return 0;
}

static int new_dimension() {
	GROW_IF_FULL_SYS(n_dimensions, dimension_cap, dimension_names);

	dimension_names[n_dimensions] = DIMENSION_UNNAMED;
	return n_dimensions++;
}

static int unit_eq(Unit *left, Unit *right) {
	if (left->n_terms != right->n_terms)
		return 0;

	if (left->scale.numerator != right->scale.numerator 
			|| left->scale.denominator != right->scale.denominator)
		return 0;


	for (int i = 0; i < left->n_terms; i++)
		if (left->terms[i].dimension != right->terms[i].dimension
				|| left->terms[i].power.numerator != right->terms[i].power.numerator
				|| left->terms[i].power.denominator != right->terms[i].power.denominator)
			return 0;

	return 1;
}

static int unit_intern(DimTerm *terms, int n_terms, Rational scale) {
	if (n_terms == 0)
		return 0;

	Unit candidate = { terms, n_terms, scale, UNIT_UNNAMED };

	for (int i = 1; i < n_units; i++)
		if (unit_eq(&units[i], &candidate))
			return i;

	GROW_IF_FULL_SYS(n_units, unit_cap, units);

	units[n_units].terms = malloc(sizeof(DimTerm) * (size_t)n_terms);
	memcpy(units[n_units].terms, terms, sizeof(DimTerm) * (size_t)n_terms);
	units[n_units].n_terms = n_terms;
	units[n_units].scale = scale;
	units[n_units].name = UNIT_UNNAMED;

	return n_units++;
}

static int unit_of_dimension(int dimension) {
	DimTerm term;
	term.dimension = dimension;
	term.power = make_rational(1, 1);

	return unit_intern(&term, 1, make_rational(1, 1));
}

static int unit_canonicalize(DimTerm *terms, int n_terms) {
	int n_kept_terms = 0;
	for (int i = 0; i < n_terms; i++) 
		if (terms[i].power.numerator != 0)
			terms[n_kept_terms++] = terms[i];

	for (int i = 1; i < n_kept_terms; i++) {
		DimTerm term = terms[i];
		int j = i - 1;

		while (j >= 0 && terms[j].dimension > term.dimension) {
			terms[j + 1] = terms[j];
			j--;
		}

		terms[j + 1] = term;
	}

	return n_kept_terms;
}

int unit_conversion(int from, int to, double *factor) {
	Unit *source = &units[from];
	Unit *target = &units[to];

	if (source->n_terms != target->n_terms)
		return 0;

	for (int i = 0; i < source->n_terms; i++)
		if (source->terms[i].dimension != target->terms[i].dimension
				|| source->terms[i].power.numerator != target->terms[i].power.numerator
				|| source->terms[i].power.denominator != target->terms[i].power.denominator)
			return 0;

	*factor = ((double)source->scale.numerator / source->scale.denominator)
		/ ((double)target->scale.numerator / target->scale.denominator);

	return 1;
}

int unit_is_named(int unit) {
	return units[unit].name != UNIT_UNNAMED;
}

double unit_scale_value(int unit) {
	return (double)units[unit].scale.numerator / units[unit].scale.denominator;
}

int unit_id_valid(int unit) {
	return unit >= 0 && unit < n_units;
}

void dimension_save(FILE *file) {
	w_i32(file, n_dimensions - init_n_dimensions);
	for (int i = init_n_dimensions; i < n_dimensions; i++)
		w_i32(file, dimension_names[i]);

	w_i32(file, n_units - init_n_units);
	for (int i = init_n_units; i < n_units; i++) {
		w_i32(file, units[i].n_terms);
		for (int t = 0; t < units[i].n_terms; t++) {
			w_i32(file, units[i].terms[t].dimension);
			w_i32(file, units[i].terms[t].power.numerator);
			w_i32(file, units[i].terms[t].power.denominator);
		}
		w_i32(file, units[i].scale.numerator);
		w_i32(file, units[i].scale.denominator);
		w_i32(file, units[i].name);
	}
}

int dimension_load(FILE *file) {
	for (int i = init_n_units; i < n_units; i++)
		free(units[i].terms);
	n_dimensions = init_n_dimensions;
	n_units = init_n_units;

	int32_t user_dims;
	if (!r_i32(file, &user_dims) || user_dims < 0)
		return 0;
	for (int i = 0; i < user_dims; i++) {
		int32_t name;
		if (!r_i32(file, &name))
			return 0;
		GROW_IF_FULL_SYS(n_dimensions, dimension_cap, dimension_names);
		dimension_names[n_dimensions++] = name;
	}

	int32_t user_units;
	if (!r_i32(file, &user_units) || user_units < 0)
		return 0;
	for (int i = 0; i < user_units; i++) {
		int32_t n_terms;
		if (!r_i32(file, &n_terms) || n_terms < 0)
			return 0;

		DimTerm *terms = malloc(sizeof(DimTerm) * (size_t)(n_terms > 0 ? n_terms : 1));
		for (int t = 0; t < n_terms; t++) {
			int32_t dimension, numerator, denominator;
			if (!r_i32(file, &dimension) || !r_i32(file, &numerator) || !r_i32(file, &denominator)) {
				free(terms);
				return 0;
			}
			terms[t].dimension = dimension;
			terms[t].power.numerator = numerator;
			terms[t].power.denominator = denominator;
		}

		int32_t scale_numerator, scale_denominator, name;
		if (!r_i32(file, &scale_numerator) || !r_i32(file, &scale_denominator) || !r_i32(file, &name)) {
			free(terms);
			return 0;
		}

		GROW_IF_FULL_SYS(n_units, unit_cap, units);
		units[n_units].terms = terms;
		units[n_units].n_terms = n_terms;
		units[n_units].scale.numerator = scale_numerator;
		units[n_units].scale.denominator = scale_denominator;
		units[n_units].name = name;
		n_units++;
	}
	return 1;
}

static int unit_combine(int left, int right, int sign) {
	Unit *left_unit = &units[left];
	Unit *right_unit = &units[right];

	int max_terms = left_unit->n_terms + right_unit->n_terms;
	DimTerm inline_terms[16];
	DimTerm *merged = max_terms <= 16 ? inline_terms : malloc(sizeof(DimTerm) * (size_t)max_terms);
	int n_merged = 0;

	for (int i = 0; i < left_unit->n_terms; i++)
		merged[n_merged++] = left_unit->terms[i];

	for (int i = 0; i < right_unit->n_terms; i++) {
		Rational power = right_unit->terms[i].power;
		if (sign < 0)
			power.numerator = -power.numerator;

		int dimension_present = 0;
		for (int j = 0; j < n_merged; j++)
			if (merged[j].dimension == right_unit->terms[i].dimension) {
				if (!rational_add(merged[j].power, power, &merged[j].power)) {
					if (merged != inline_terms) free(merged);
					return -1;
				}
				dimension_present = 1;
				break;
			}

		if (!dimension_present) {
			merged[n_merged].dimension = right_unit->terms[i].dimension;
			merged[n_merged].power = power;
			n_merged++;
		}
	}

	int n_terms = unit_canonicalize(merged, n_merged);

	Rational scale;
	int scale_ok = sign > 0
		? rational_multiply(left_unit->scale, right_unit->scale, &scale)
		: rational_divide(left_unit->scale, right_unit->scale, &scale);
	if (!scale_ok) {
		if (merged != inline_terms) free(merged);
		return -1;
	}

	int combined_unit = unit_intern(merged, n_terms, scale);

	if (merged != inline_terms) free(merged);
	return combined_unit;
}

int unit_multiply(Interpreter *interp, int left, int right) {
	int combined = unit_combine(left, right, 1);
	if (combined < 0)
		fail(interp, "*: unit scale or exponent overflow");
	return combined;
}

int unit_divide(Interpreter *interp, int left, int right) {
	int combined = unit_combine(left, right, -1);
	if (combined < 0)
		fail(interp, "/: unit scale or exponent overflow");
	return combined;
}

int unit_pow(Interpreter *interp, int unit, int numerator, int denominator) {
	Rational exponent = make_rational(numerator, denominator);

	int success;
	Rational scale = rational_pow(units[unit].scale, exponent, &success);
	if (!success) {
		fail(interp, "unit: cannot raise a scaled unit to the power %d/%d exactly (irrational root or scale)",
				exponent.numerator, exponent.denominator);
		return -1;
	}

	Unit *base_unit = &units[unit];
	DimTerm *scaled_terms = malloc(sizeof(DimTerm) * (size_t)base_unit->n_terms);

	for (int i = 0; i < base_unit->n_terms; i++) {
		scaled_terms[i].dimension = base_unit->terms[i].dimension;
		if (!rational_multiply(base_unit->terms[i].power, exponent, &scaled_terms[i].power)) {
			free(scaled_terms);
			fail(interp, "unit: exponent overflow raising to %d/%d", numerator, denominator);
			return -1;
		}
	}

	int n_terms = unit_canonicalize(scaled_terms, base_unit->n_terms);
	int raised_unit = unit_intern(scaled_terms, n_terms, scale);

	free(scaled_terms);
	return raised_unit;
}

void render_unit(FILE *out, int unit) {
	if (units[unit].name != UNIT_UNNAMED) {
		fputs(&vocab.name_pool[units[unit].name], out);
		return;
	}

	int printed = 0;
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < units[unit].n_terms; i++) {
			DimTerm term = units[unit].terms[i];
			int positive = term.power.numerator > 0;
			if (pass == 0 ? !positive : positive)
				continue;

			if (printed)
				fputc('.', out);
			printed = 1;

			fputs(&vocab.name_pool[dimension_names[term.dimension]], out);

			if (term.power.numerator == 1 && term.power.denominator == 1)
				continue;

			if (term.power.denominator == 1)
				fprintf(out, "^%d", term.power.numerator);
			else
				fprintf(out, "^%d/%d", term.power.numerator, term.power.denominator);
		}
	}
}

void push_quantity(Interpreter *interp, Val magnitude, int unit) {
	if (unit == 0) {
		push(interp, magnitude);
		return;
	}

	gc_root_push(interp, magnitude);
	int slot = object_new_pair(interp);
	gc_root_pop(interp);
	if (interp->error_flag) return;

	pairs.table[slot].head = magnitude;
	pairs.table[slot].tail.bits = (uint64_t)unit;

	push(interp, make_quantity(slot));
}

int quantity_truthy(Val quantity) {
	return truthy(pairs.table[VAL_DATA(quantity)].head);
}

void p_base(Interpreter *interp) {
	int dimension = new_dimension();
	int unit = unit_of_dimension(dimension);

	push_quantity(interp, make_float(1.0), unit);

	DISPATCH(interp);
}

void apply_unit(Interpreter *interp, int cfa) {
	int unit = (int)vocab.dict[cfa + 1];

	POP(magnitude);
	if (VAL_TAG(magnitude) != T_FLOAT && VAL_TAG(magnitude) != T_MATRIX) {
		fail(interp, "unit: expected a number or matrix");
		return;
	}

	push_quantity(interp, magnitude, unit);
}

void dounit(Interpreter *interp) {
	int cfa = (int)vocab.dict[interp->ip++];

	apply_unit(interp, cfa);
	if (interp->error_flag) return;

	DISPATCH(interp);
}

void p_unit(Interpreter *interp) {
	char *name = next_token();
	if (!name || !*name) {
		fail(interp, "unit: expected a name");
		return;
	}

	POP_QUANTITY(quantity, "unit");
	int source_unit = (int)quantity->tail.bits;
	Val magnitude = quantity->head;
	if (VAL_TAG(magnitude) != T_FLOAT) {
		fail(interp, "unit: scale must be a scalar number; got %s", tag_name(VAL_TAG(magnitude)));
		return;
	}

	double value = VAL_NUMBER(magnitude);
	Rational scale;
	if (!rational_of_double(value, &scale)) {
		fail(interp, "unit: scale must be a positive simple rational of the base unit; got %g", value);
		return;
	}
	if (!rational_multiply(scale, units[source_unit].scale, &scale)) {
		fail(interp, "unit: scale overflow combining with the base unit");
		return;
	}

	int base_dimension = DIMENSION_UNNAMED;
	if (units[source_unit].n_terms == 1
			&& units[source_unit].terms[0].power.numerator == 1
			&& units[source_unit].terms[0].power.denominator == 1
			&& dimension_names[units[source_unit].terms[0].dimension] == DIMENSION_UNNAMED)
		base_dimension = units[source_unit].terms[0].dimension;

	int unit = unit_intern(units[source_unit].terms, units[source_unit].n_terms, scale);

	int cfa = create_header(interp, name, 0);
	units[unit].name = WORD_NAME(cfa);
	if (base_dimension != DIMENSION_UNNAMED)
		dimension_names[base_dimension] = WORD_NAME(cfa);

	emit(interp, (cell)dounit);
	emit(interp, (cell)unit);

	DISPATCH(interp);
}
