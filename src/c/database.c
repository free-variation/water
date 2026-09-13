#include "telic.h"
#include "sqlite3.h"

#define DB_SLOT_BITS 8
#define DB_SLOT_MASK ((1 << DB_SLOT_BITS) - 1)

static int db_slot(Val db_val) {
	return (int)(VAL_DATA(db_val) & DB_SLOT_MASK);
}

static sqlite3 *db_connection(Interpreter *interp, Val db_val) {
	int slot = db_slot(db_val);
	if ((int)(VAL_DATA(db_val) >> DB_SLOT_BITS) != interp->database_generation[slot])
		return NULL;

	return interp->databases[slot];
}

#define POP_DB(name, op) \
      POP_TYPED(name, op, T_DB); \
      sqlite3 *name = db_connection(interp, name##_val)

void p_db_open(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val path_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(path_val, T_STRING, "db-open", "a string");
	Object *path = OBJECT_AT(VAL_DATA(path_val));

	int slot = -1;
	for (int i = 0; i < MAX_DATABASES; i++)
		if (!interp->databases[i]) {
			slot = i;
			break;
		}

	if (slot < 0) {
		fail(interp, "too many open databases (max %d)", MAX_DATABASES);
		return;
	}

	sqlite3 *db;
	if (sqlite3_open(path->bytes, &db) != SQLITE_OK) {
		fail(interp, "%s", sqlite3_errmsg(db));
		sqlite3_close(db);
		return;
	}

	interp->databases[slot] = db;
	if (slot >= interp->n_databases)
		interp->n_databases = slot + 1;

	chain_sp[-1] = make_db(slot | (interp->database_generation[slot] << DB_SLOT_BITS));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_db_close(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val db_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(db_val, T_DB, "db-close", "a database");
	sqlite3 *db = db_connection(interp, db_val);

	if (db) {
		sqlite3_close(db);
		interp->databases[db_slot(db_val)] = NULL;
		interp->database_generation[db_slot(db_val)]++;
	}

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

static int db_bind(Interpreter *interp, sqlite3_stmt *statement, Object *params) {
	int expected = sqlite3_bind_parameter_count(statement);
	if (params->len != expected) {
		fail(interp, "query takes %d parameter(s) %d given", expected, params->len);
		return -1;
	}

	for (int i = 0; i < params->len; i++) {
		Val value = params->items[i];

		switch (VAL_TAG(value)) {
			case T_FLOAT:
				sqlite3_bind_double(statement, i + 1, VAL_NUMBER(value));
				break;
			case T_NONE:
				sqlite3_bind_null(statement, i + 1);
				break;
			case T_STRING: {
							   Object *text = OBJECT_AT(VAL_DATA(value));
							   sqlite3_bind_text(statement, i + 1, text->bytes, text->len, SQLITE_TRANSIENT);
							   break;
						   }
			case T_SYMBOL:
						   sqlite3_bind_text(statement, i + 1, &vocab.symbol_pool[VAL_DATA(value)], -1, SQLITE_TRANSIENT);
						   break;
			case T_EXACT: {
							  int64_t integer;
							  if (!exact_fits_int64(value, &integer)) {
								  fail(interp, "cannot bind a non-integer or oversized exact as a parameter");
								  return -1;
							  }
							  sqlite3_bind_int64(statement, i + 1, integer);
							  break;
						  }
			default:
						   fail(interp, "cannot bind %s as a parameter", tag_name(VAL_TAG(value)));
						   return -1;
		}
	}

	return 0;
}

static sqlite3_stmt *db_prepare_bound(Interpreter *interp, sqlite3 *db, Object *sql, Object *params) {
	if (!db) {
		fail(interp, "database is closed");
		return NULL;
	}

	sqlite3_stmt *prepared;
	if (sqlite3_prepare_v2(db, sql->bytes, sql->len, &prepared, NULL) != SQLITE_OK) {
		fail(interp, "%s", sqlite3_errmsg(db));
		return NULL;
	}
	if (db_bind(interp, prepared, params) != 0) {
		sqlite3_finalize(prepared);
		return NULL;
	}

	return prepared;
}

static int db_step_rows(Interpreter *interp, sqlite3 *db, sqlite3_stmt *statement,
		int (*consume_row)(Interpreter *interp, sqlite3_stmt *statement, void *context),
		void *context) {
	int status;
	while ((status = sqlite3_step(statement)) == SQLITE_ROW)
		if (consume_row && consume_row(interp, statement, context) != 0)
			break;

	sqlite3_finalize(statement);

	if (!interp->error_flag && status != SQLITE_DONE)
		fail(interp, "%s", sqlite3_errmsg(db));

	return interp->error_flag ? -1 : 0;
}

void p_db_exec(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	Val params_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(params_val, T_ARRAY, "db-exec", "an array");
	Object *params = OBJECT_AT(VAL_DATA(params_val));
	Val statement_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(statement_val, T_STRING, "db-exec", "a string");
	Object *statement = OBJECT_AT(VAL_DATA(statement_val));
	Val db_val = chain_sp[-3];
	REQUIRE_CHAIN_TAG(db_val, T_DB, "db-exec", "a database");
	sqlite3 *db = db_connection(interp, db_val);

	sqlite3_stmt *prepared = db_prepare_bound(interp, db, statement, params);
	if (!prepared)
		return;

	if (db_step_rows(interp, db, prepared, NULL, NULL) != 0)
		return;

	chain_sp[-3] = make_float((double)sqlite3_changes(db));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

static Val db_column_value(Interpreter *interp, sqlite3_stmt *statement, int column) {
	switch (sqlite3_column_type(statement, column)) {
		case SQLITE_INTEGER: {
					 int64_t integer = sqlite3_column_int64(statement, column);
					 if (integer > (int64_t)1 << 53 || integer < -((int64_t)1 << 53))
						 return exact_from_int64(interp, integer);
					 return make_float((double)integer);
				 }
		case SQLITE_FLOAT:
			return make_float(sqlite3_column_double(statement, column));
		case SQLITE_NULL:
			return make_tagged(T_NONE, 0);
		default: {
					 const char *bytes = (const char *)sqlite3_column_blob(statement, column);
					 int length = sqlite3_column_bytes(statement, column);
					 return make_string(object_new_string(interp, bytes ? bytes : "", length));
				 }
	}
}

#define DB_QUERY_OPERANDS(word) \
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3); \
	Val params_val = chain_sp[-1]; \
	REQUIRE_CHAIN_TAG(params_val, T_ARRAY, word, "an array"); \
	Val query_val = chain_sp[-2]; \
	REQUIRE_CHAIN_TAG(query_val, T_STRING, word, "a string"); \
	Val db_val = chain_sp[-3]; \
	REQUIRE_CHAIN_TAG(db_val, T_DB, word, "a database"); \
	sqlite3 *db = db_connection(interp, db_val); \
	\
	sqlite3_stmt *statement = db_prepare_bound(interp, db, \
			OBJECT_AT(VAL_DATA(query_val)), OBJECT_AT(VAL_DATA(params_val))); \
	if (!statement) \
		return

typedef struct {
	const int *column_handles;
	int n_columns;
	char *column_numeric;
	char *column_any_float;
} DatasetColumnsContext;

static int dataset_consume_row(Interpreter *interp, sqlite3_stmt *statement, void *context) {
	DatasetColumnsContext *columns_context = context;

	int n_columns = columns_context->n_columns;
	for (int j = 0; j < n_columns; j++) {
		int column_handle = columns_context->column_handles[j];
		if (column_handle < 0)
			continue;

		Val value = db_column_value(interp, statement, j);
		if (interp->error_flag)
			return -1;

		if (VAL_TAG(value) == T_FLOAT)
			columns_context->column_any_float[j] = 1;
		else if (VAL_TAG(value) != T_NONE)
			columns_context->column_numeric[j] = 0;

		Object *column = OBJECT_AT(column_handle);
		ITEMS_GROW_IF_FULL(column);
		column->items[column->len++] = value;
	}

	return 0;
}

static int symbol_repeats_later(const cell *keys, int position, int n_columns) {
	for (int k = position + 1; k < n_columns; k++)
		if (keys[k] == keys[position])
			return 1;
	return 0;
}

static __attribute__((noinline)) int db_query_dataset_build(Interpreter *interp, sqlite3 *db, sqlite3_stmt *statement, int *types_handle_out) {
	*types_handle_out = 0;

	int n_columns = sqlite3_column_count(statement);
	cell keys[n_columns];
	for (int j = 0; j < n_columns; j++)
		keys[j] = intern_symbol(interp, sqlite3_column_name(statement, j));

	int dataset_handle = object_new_frame(interp);
	if (interp->error_flag) {
		sqlite3_finalize(statement);
		return -1;
	}
	gc_root_push(interp, make_frame(dataset_handle));

	int types_handle = object_new_frame(interp);
	if (interp->error_flag) {
		sqlite3_finalize(statement);
		gc_root_pop(interp);
		return -1;
	}
	gc_root_push(interp, make_frame(types_handle));

	for (int j = 0; j < n_columns && !interp->error_flag; j++) {
		const char *declared_type = sqlite3_column_decltype(statement, j);
		Val declaration = make_tagged(T_NONE, 0);
		if (declared_type)
			declaration = make_string(object_new_string(interp, declared_type, (int)strlen(declared_type)));
		frame_put(OBJECT_AT(types_handle), keys[j], declaration);
	}

	int column_handles[n_columns];
	char column_numeric[n_columns];
	char column_any_float[n_columns];
	for (int j = 0; j < n_columns && !interp->error_flag; j++) {
		column_numeric[j] = 1;
		column_any_float[j] = 0;
		if (symbol_repeats_later(keys, j, n_columns)) {
			column_handles[j] = -1;
			continue;
		}

		column_handles[j] = object_new_array(interp, 0);
		if (interp->error_flag)
			break;
		frame_put(OBJECT_AT(dataset_handle), keys[j], make_array(column_handles[j]));
	}
	if (interp->error_flag) {
		sqlite3_finalize(statement);
		gc_root_pop(interp);
		gc_root_pop(interp);
		return -1;
	}

	DatasetColumnsContext columns_context = {
		.column_handles = column_handles,
		.n_columns = n_columns,
		.column_numeric = column_numeric,
		.column_any_float = column_any_float
	};
	if (db_step_rows(interp, db, statement, dataset_consume_row, &columns_context) != 0) {
		gc_root_pop(interp);
		gc_root_pop(interp);
		return -1;
	}

	for (int j = 0; j < n_columns; j++) {
		if (column_handles[j] < 0 || !column_numeric[j] || !column_any_float[j])
			continue;

		Object *column = OBJECT_AT(column_handles[j]);
		int vector_handle = object_new_matrix(interp, column->len, 1);
		if (interp->error_flag) {
			gc_root_pop(interp);
			gc_root_pop(interp);
			return -1;
		}

		Object *vector = OBJECT_AT(vector_handle);
		for (int i = 0; i < column->len; i++) {
			Val element = column->items[i];
			vector->matrix.elements[i] = VAL_TAG(element) == T_NONE ? NAN : VAL_NUMBER(element);
		}
		frame_put(OBJECT_AT(dataset_handle), keys[j], make_matrix(vector_handle));
	}

	gc_root_pop(interp);
	gc_root_pop(interp);
	*types_handle_out = types_handle;
	return dataset_handle;
}

void p_db_query(DISPATCH_ARGS) {
	DB_QUERY_OPERANDS("(db-query)");

	int types_handle;
	int dataset_handle = db_query_dataset_build(interp, db, statement, &types_handle);
	if (interp->error_flag)
		return;

	chain_sp[-3] = make_frame(dataset_handle);
	chain_sp[-2] = make_frame(types_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}
