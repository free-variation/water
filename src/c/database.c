#include "water.h"
#include "sqlite3.h"

#define POP_DB(name, op) \
      POP_TYPED(name, op, T_DB); \
      sqlite3 *name = interp->databases[VAL_DATA(name##_val)]

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

	chain_sp[-1] = make_db(slot);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_db_close(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val db_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(db_val, T_DB, "db-close", "a database");
	sqlite3 *db = interp->databases[VAL_DATA(db_val)];

	if (db) {
		sqlite3_close(db);
		interp->databases[(int)VAL_DATA(db_val)] = NULL;
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
	sqlite3 *db = interp->databases[VAL_DATA(db_val)];

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
		case SQLITE_INTEGER:
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

static int db_build_row(Interpreter *interp, sqlite3_stmt *statement, const cell *keys, int columns) {
	int row_handle = object_new_frame(interp);
	if (interp->error_flag)
		return -1;
	gc_root_push(interp, make_frame(row_handle));

	for (int j = 0; j < columns && !interp->error_flag; j++) {
		Val value = db_column_value(interp, statement, j);
		frame_put(OBJECT_AT(row_handle), keys[j], value);
	}

	gc_root_pop(interp);

	return row_handle;
}

typedef struct {
	const cell *keys;
	int n_columns;
	int rows_handle;
} RelationRowsContext;

static int relation_consume_row(Interpreter *interp, sqlite3_stmt *statement, void *context) {
	RelationRowsContext *rows_context = context;

	int row_handle = db_build_row(interp, statement, rows_context->keys, rows_context->n_columns);
	if (interp->error_flag) return -1;

	Object *rows = OBJECT_AT(rows_context->rows_handle);
	GROW_IF_FULL(rows->len, rows->capacity, rows->items);
	rows->items[rows->len++] = make_frame(row_handle);
	return 0;
}

#define DB_QUERY_OPERANDS(word) \
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3); \
	Val params_val = chain_sp[-1]; \
	REQUIRE_CHAIN_TAG(params_val, T_ARRAY, word, "an array"); \
	Val query_val = chain_sp[-2]; \
	REQUIRE_CHAIN_TAG(query_val, T_STRING, word, "a string"); \
	Val db_val = chain_sp[-3]; \
	REQUIRE_CHAIN_TAG(db_val, T_DB, word, "a database"); \
	sqlite3 *db = interp->databases[VAL_DATA(db_val)]; \
	\
	sqlite3_stmt *statement = db_prepare_bound(interp, db, \
			OBJECT_AT(VAL_DATA(query_val)), OBJECT_AT(VAL_DATA(params_val))); \
	if (!statement) \
		return; \
	\
	int n_columns = sqlite3_column_count(statement); \
	cell keys[n_columns]; \
	for (int j = 0; j < n_columns; j++) \
		keys[j] = intern_symbol(interp, sqlite3_column_name(statement, j))

void p_db_query(DISPATCH_ARGS) {
	DB_QUERY_OPERANDS("db-query");

	int rows_handle = object_new_array(interp, 0);
	if (interp->error_flag) {
		sqlite3_finalize(statement);
		return;
	}
	gc_root_push(interp, make_array(rows_handle));

	RelationRowsContext rows_context = {
		.keys = keys,
		.n_columns = n_columns,
		.rows_handle = rows_handle
	};
	if (db_step_rows(interp, db, statement, relation_consume_row, &rows_context) != 0) {
		gc_root_pop(interp);
		return;
	}

	int index_handle = object_new_frame(interp);
	if (interp->error_flag) {
		gc_root_pop(interp);
		return;
	}
	gc_root_push(interp, make_frame(index_handle));

	int relation_handle = object_new_frame(interp);
	if (interp->error_flag) {
		gc_root_pop(interp);
		gc_root_pop(interp);
		return;
	}

	Object *relation = OBJECT_AT(relation_handle);
	frame_put(relation, intern_symbol(interp, "rows"), make_array(rows_handle));
	frame_put(relation, intern_symbol(interp, "index"), make_frame(index_handle));

	gc_root_pop(interp);
	gc_root_pop(interp);
	chain_sp[-3] = make_frame(relation_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

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
		GROW_IF_FULL(column->len, column->capacity, column->items);
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

void p_db_query_to_dataset(DISPATCH_ARGS) {
	DB_QUERY_OPERANDS("(db-query>dataset)");

	int dataset_handle = object_new_frame(interp);
	if (interp->error_flag) {
		sqlite3_finalize(statement);
		return;
	}
	gc_root_push(interp, make_frame(dataset_handle));

	int types_handle = object_new_frame(interp);
	if (interp->error_flag) {
		sqlite3_finalize(statement);
		gc_root_pop(interp);
		return;
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
		return;
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
		return;
	}

	for (int j = 0; j < n_columns; j++) {
		if (column_handles[j] < 0 || !column_numeric[j] || !column_any_float[j])
			continue;

		Object *column = OBJECT_AT(column_handles[j]);
		int vector_handle = object_new_matrix(interp, column->len, 1);
		if (interp->error_flag) {
			gc_root_pop(interp);
			gc_root_pop(interp);
			return;
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
	chain_sp[-3] = make_frame(dataset_handle);
	chain_sp[-2] = make_frame(types_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}
