#include "logicforth.h"
#include "sqlite3.h"

#define POP_DB(name, op) \
      Val name##_val = pop(interp); \
      if (interp->error_flag) return; \
      if (VAL_TAG(name##_val) != T_DB) { \
              fail(interp, "%s: expected a database; got %s", (op), tag_name(VAL_TAG(name##_val))); \
              return; \
      } \
      sqlite3 *name = interp->databases[VAL_DATA(name##_val)]

void p_db_open(Interpreter *interp) {
	POP_STRING(path, "db-open");

	int slot = -1;
	for (int i = 0; i < MAX_DATABASES; i++) 
		if (!interp->databases[i]) {
			slot = i;
			break;
		}

	if (slot < 0) {
		fail(interp, "db-open too many open databases (max %d)", MAX_DATABASES);
		return;
	}

	sqlite3 *db;
	if (sqlite3_open(path->bytes, &db) != SQLITE_OK) {
		fail(interp, "db-open: %s", sqlite3_errmsg(db));
		sqlite3_close(db);
		return;
	}

	interp->databases[slot] = db;
	if (slot >= interp->n_databases) 
		interp->n_databases = slot + 1;

	push (interp, make_db(slot));

	DISPATCH(interp);
}

void p_db_close(Interpreter *interp) {
	POP_DB(db, "db-close");

	if (db) {
		sqlite3_close(db);
		interp->databases[(int)VAL_DATA(db_val)] = NULL;
	}

	DISPATCH(interp);
}

static int db_bind(Interpreter *interp, sqlite3_stmt *statement, Object *params, const char *op) {
	int expected = sqlite3_bind_parameter_count(statement);
	if (params->len != expected) {
		fail(interp, "%s: query takes %d parameter(s) %d given", op, expected, params->len);
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
						   fail(interp, "%s: cannot bind %s as a parameter", op, tag_name(VAL_TAG(value)));
						   return -1;
		}
	}

	return 0;
}

void p_db_exec(Interpreter *interp) {
	POP_ARRAY(params, "db-exec");
	POP_STRING(statement, "db-exec");
	POP_DB(db, "db-exec");
	if (!db) {
		fail(interp, "db-exec: database is closed");
		return;
	}

	sqlite3_stmt *prepared;
	if (sqlite3_prepare_v2(db, statement->bytes, statement->len, &prepared, NULL) != SQLITE_OK) {
		fail(interp, "db-exec: %s", sqlite3_errmsg(db));
		return;
	}
	if (db_bind(interp, prepared, params, "db-exec") != 0) {
		sqlite3_finalize(prepared);
		return;
	}

	int status;
	while ((status = sqlite3_step(prepared)) == SQLITE_ROW)
		;
	int affected = sqlite3_changes(db);
	sqlite3_finalize(prepared);
	if (status != SQLITE_DONE) {
		fail(interp, "db-exec: %s", sqlite3_errmsg(db));
		return;
	}

	push(interp, make_float((double)affected));

	DISPATCH(interp);
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

void p_db_query(Interpreter *interp) {
	POP_ARRAY(params, "db-query");
	POP_STRING(query, "db-query");
	POP_DB(db, "db-query");
	if (!db) {
		fail(interp, "db-query: database is closed");
		return;
	}

	sqlite3_stmt *statement;
	if (sqlite3_prepare_v2(db, query->bytes, query->len, &statement, NULL) != SQLITE_OK) {
		fail(interp, "db-query: %s", sqlite3_errmsg(db));
		return;
	}

	if (db_bind(interp, statement, params, "db-query") != 0) {
		sqlite3_finalize(statement);
		return;
	}

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
		fail(interp, "db-query: %s", sqlite3_errmsg(db));
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
	push(interp, make_frame(relation_handle));

	DISPATCH(interp);
}

	



