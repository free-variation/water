#include "water.h"
#include "sqlite3.h"

#define POP_DB(name, op) \
      POP_TYPED(name, op, T_DB); \
      sqlite3 *name = interp->databases[VAL_DATA(name##_val)]

void p_db_open(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val path_val = chain_sp[-1];
	if (VAL_TAG(path_val) != T_STRING) {
		fail(interp, "expected %s; got %s", tag_name(T_STRING), tag_name(VAL_TAG(path_val)));
		return;
	}
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
	if (VAL_TAG(db_val) != T_DB) {
		fail(interp, "expected %s; got %s", tag_name(T_DB), tag_name(VAL_TAG(db_val)));
		return;
	}
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

void p_db_exec(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	Val params_val = chain_sp[-1];
	if (VAL_TAG(params_val) != T_ARRAY) {
		fail(interp, "expected %s; got %s", tag_name(T_ARRAY), tag_name(VAL_TAG(params_val)));
		return;
	}
	Object *params = OBJECT_AT(VAL_DATA(params_val));
	Val statement_val = chain_sp[-2];
	if (VAL_TAG(statement_val) != T_STRING) {
		fail(interp, "expected %s; got %s", tag_name(T_STRING), tag_name(VAL_TAG(statement_val)));
		return;
	}
	Object *statement = OBJECT_AT(VAL_DATA(statement_val));
	Val db_val = chain_sp[-3];
	if (VAL_TAG(db_val) != T_DB) {
		fail(interp, "expected %s; got %s", tag_name(T_DB), tag_name(VAL_TAG(db_val)));
		return;
	}
	sqlite3 *db = interp->databases[VAL_DATA(db_val)];

	sqlite3_stmt *prepared = db_prepare_bound(interp, db, statement, params);
	if (!prepared)
		return;

	int status;
	while ((status = sqlite3_step(prepared)) == SQLITE_ROW)
		;
	int affected = sqlite3_changes(db);
	sqlite3_finalize(prepared);
	if (status != SQLITE_DONE) {
		fail(interp, "%s", sqlite3_errmsg(db));
		return;
	}

	chain_sp[-3] = make_float((double)affected);

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

void p_db_query(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	Val params_val = chain_sp[-1];
	if (VAL_TAG(params_val) != T_ARRAY) {
		fail(interp, "expected %s; got %s", tag_name(T_ARRAY), tag_name(VAL_TAG(params_val)));
		return;
	}
	Object *params = OBJECT_AT(VAL_DATA(params_val));
	Val query_val = chain_sp[-2];
	if (VAL_TAG(query_val) != T_STRING) {
		fail(interp, "expected %s; got %s", tag_name(T_STRING), tag_name(VAL_TAG(query_val)));
		return;
	}
	Object *query = OBJECT_AT(VAL_DATA(query_val));
	Val db_val = chain_sp[-3];
	if (VAL_TAG(db_val) != T_DB) {
		fail(interp, "expected %s; got %s", tag_name(T_DB), tag_name(VAL_TAG(db_val)));
		return;
	}
	sqlite3 *db = interp->databases[VAL_DATA(db_val)];

	sqlite3_stmt *statement = db_prepare_bound(interp, db, query, params);
	if (!statement)
		return;

	int n_columns = sqlite3_column_count(statement);
	cell keys[n_columns];
	for (int j = 0; j < n_columns; j++)
		keys[j] = intern_symbol(interp, sqlite3_column_name(statement, j));

	int rows_handle = object_new_array(interp, 0);
	if (interp->error_flag) {
		sqlite3_finalize(statement);
		return;
	}
	gc_root_push(interp, make_array(rows_handle));

	int status;
	Object *rows = OBJECT_AT(rows_handle);
	while ((status = sqlite3_step(statement)) == SQLITE_ROW) {
		int row_handle = db_build_row(interp, statement, keys, n_columns);
		if (interp->error_flag)
			break;

		GROW_IF_FULL(rows->len, rows->capacity, rows->items);
		rows->items[rows->len++] = make_frame(row_handle);
	}
	sqlite3_finalize(statement);

	if (!interp->error_flag && status != SQLITE_DONE)
		fail(interp, "%s", sqlite3_errmsg(db));
	if (interp->error_flag) {
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

