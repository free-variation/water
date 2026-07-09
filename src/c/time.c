#include "water.h"

void p_now(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);

	struct timespec monotonic_time;
	clock_gettime(CLOCK_MONOTONIC, &monotonic_time);

	chain_sp[0] = make_float((double)monotonic_time.tv_sec + (double)monotonic_time.tv_nsec / 1e9);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_wall_now(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);

	struct timespec wall_time;
	clock_gettime(CLOCK_REALTIME, &wall_time);

	chain_sp[0] = make_float((double)wall_time.tv_sec + (double)wall_time.tv_nsec / 1e9);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

static void refresh_timezone(void) {
#ifndef __wasi__
	tzset();
#endif
}

static int date_frame_from_calendar(Interpreter *interp, struct tm *calendar, double second_fraction) {
	int handle = object_new_frame(interp);
	if (interp->error_flag)
		return -1;
	Object *date = OBJECT_AT(handle);

	frame_put(date, intern_symbol(interp, "year"),    make_float((double)calendar->tm_year + 1900));
	frame_put(date, intern_symbol(interp, "month"),   make_float((double)calendar->tm_mon + 1));
	frame_put(date, intern_symbol(interp, "day"),     make_float((double)calendar->tm_mday));
	frame_put(date, intern_symbol(interp, "hour"),    make_float((double)calendar->tm_hour));
	frame_put(date, intern_symbol(interp, "minute"),  make_float((double)calendar->tm_min));
	frame_put(date, intern_symbol(interp, "second"),  make_float((double)calendar->tm_sec + second_fraction));
	frame_put(date, intern_symbol(interp, "weekday"), make_float((double)calendar->tm_wday));
	frame_put(date, intern_symbol(interp, "yearday"), make_float((double)calendar->tm_yday + 1));

	return handle;
}

void p_epoch_to_date(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	SYNC_REGISTERS(interp, chain_ip, chain_sp);

	Val epoch_val = chain_sp[-1];
	if (VAL_TAG(epoch_val) != T_FLOAT) {
		fail(interp, "(epoch>date): expected a float epoch; got %s", tag_name(VAL_TAG(epoch_val)));
		return;
	}

	double epoch = VAL_NUMBER(epoch_val);
	double whole = floor(epoch);
	time_t moment = (time_t)whole;
	struct tm calendar;
	gmtime_r(&moment, &calendar);

	int handle = date_frame_from_calendar(interp, &calendar, epoch - whole);
	if (handle < 0)
		return;

	chain_sp[-1] = make_frame(handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_epoch_to_date_local(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	SYNC_REGISTERS(interp, chain_ip, chain_sp);

	Val epoch_val = chain_sp[-1];
	if (VAL_TAG(epoch_val) != T_FLOAT) {
		fail(interp, "(epoch>date-local): expected a float epoch; got %s", tag_name(VAL_TAG(epoch_val)));
		return;
	}

	double epoch = VAL_NUMBER(epoch_val);
	double whole = floor(epoch);
	time_t moment = (time_t)whole;
	struct tm calendar;
	refresh_timezone();
	localtime_r(&moment, &calendar);

	int handle = date_frame_from_calendar(interp, &calendar, epoch - whole);
	if (handle < 0)
		return;

	chain_sp[-1] = make_frame(handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

static long long days_from_civil(long long year, int month, int day) {
	year -= month <= 2;
	long long era = (year >= 0 ? year : year - 399) / 400;
	int year_of_era = (int)(year - era * 400);
	int day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	int day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;

	return era * 146097 + day_of_era - 719468;
}

static int date_field(Interpreter *interp, Object *date, const char *key,
		double fallback, double *out, const char *op) {
	cell key_symbol = intern_symbol(interp, key);
	FRAME_LOOKUP(date, key_symbol, at, present);

	if (!present) {
		*out = fallback;
		return 1;
	}

	Val value = date->frame.values[at];
	if (VAL_TAG(value) != T_FLOAT) {
		fail(interp, "%s: :%s must be a float; got %s", op, key, tag_name(VAL_TAG(value)));
		return 0;
	}

	*out = VAL_NUMBER(value);
	return 1;
}

static int read_date_fields(Interpreter *interp, Object *date, const char *op,
		double *year, double *month, double *day,
		double *hour, double *minute, double *second) {
	cell year_symbol = intern_symbol(interp, "year");
	FRAME_LOOKUP(date, year_symbol, at, present);
	if (!present) {
		fail(interp, "%s: the date frame needs a :year", op);
		return 0;
	}

	return date_field(interp, date, "year",   0, year,   op)
		&& date_field(interp, date, "month",  1, month,  op)
		&& date_field(interp, date, "day",    1, day,    op)
		&& date_field(interp, date, "hour",   0, hour,   op)
		&& date_field(interp, date, "minute", 0, minute, op)
		&& date_field(interp, date, "second", 0, second, op);
}

void p_date_to_epoch(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	SYNC_REGISTERS(interp, chain_ip, chain_sp);

	Val date_val = chain_sp[-1];
	if (VAL_TAG(date_val) != T_FRAME) {
		fail(interp, "(date>epoch): expected a frame; got %s", tag_name(VAL_TAG(date_val)));
		return;
	}
	Object *date = OBJECT_AT(VAL_DATA(date_val));

	double year, month, day, hour, minute, second;
	if (!read_date_fields(interp, date, "(date>epoch)", &year, &month, &day, &hour, &minute, &second))
		return;

	long long month_index = (long long)month - 1;
	long long year_carry = month_index >= 0 ? month_index / 12 : (month_index - 11) / 12;
	long long days = days_from_civil((long long)year + year_carry, 
			(int)(month_index - year_carry * 12) + 1, (int)day);

	chain_sp[-1] = make_float((double)days * 86400.0 +  hour * 3600.0 + minute * 60.0 + second);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_date_to_epoch_local(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	SYNC_REGISTERS(interp, chain_ip, chain_sp);

	Val date_val = chain_sp[-1];
	if (VAL_TAG(date_val) != T_FRAME) {
		fail(interp, "(date>epoch-local): expected a frame; got %s", tag_name(VAL_TAG(date_val)));
		return;
	}
	Object *date = OBJECT_AT(VAL_DATA(date_val));

	double year, month, day, hour, minute, second;
	if (!read_date_fields(interp, date, "(date>epoch-local)", &year, &month, &day, &hour, &minute, &second))
		return;

	struct tm calendar = {0};
	calendar.tm_year = (int)year - 1900;
	calendar.tm_mon  = (int)month - 1;
	calendar.tm_mday = (int)day;
	calendar.tm_hour = (int)hour;
	calendar.tm_min  = (int)minute;
	calendar.tm_sec  = (int)second;
	calendar.tm_isdst = -1;
	refresh_timezone();
	time_t moment = mktime(&calendar);

	chain_sp[-1] = make_float((double)moment + (second - floor(second)));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

static int format_time_at(Interpreter *interp, Val *stack_top, const char *op, int local) {
	Val format_val = stack_top[-1];
	Val epoch_val = stack_top[-2];
	if (VAL_TAG(format_val) != T_STRING) {
		fail(interp, "%s: expected a format string; got %s", op, tag_name(VAL_TAG(format_val)));
		return 0;
	}
	if (VAL_TAG(epoch_val) != T_FLOAT) {
		fail(interp, "%s: expected a float epoch; got %s", op, tag_name(VAL_TAG(epoch_val)));
		return 0;
	}

	Object *format = OBJECT_AT(VAL_DATA(format_val));
	time_t moment = (time_t)floor(VAL_NUMBER(epoch_val));
	struct tm calendar;
	if (local) {
		refresh_timezone();
		localtime_r(&moment, &calendar);
	} else
		gmtime_r(&moment, &calendar);

	char rendered[512];
	size_t length = strftime(rendered, sizeof(rendered), format->bytes, &calendar);

	int handle = object_new_string(interp, rendered, (int)length);
	if (interp->error_flag)
		return 0;

	stack_top[-2] = make_string(handle);
	return 1;
}

void p_format_time(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	SYNC_REGISTERS(interp, chain_ip, chain_sp);

	if (!format_time_at(interp, chain_sp, "(format-time)", 0))
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_format_time_local(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	SYNC_REGISTERS(interp, chain_ip, chain_sp);

	if (!format_time_at(interp, chain_sp, "(format-time-local)", 1))
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_parse_time(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	SYNC_REGISTERS(interp, chain_ip, chain_sp);

	Val format_val = chain_sp[-1];
	Val text_val = chain_sp[-2];
	if (VAL_TAG(format_val) != T_STRING) {
		fail(interp, "(parse-time): expected a format string; got %s", tag_name(VAL_TAG(format_val)));
		return;
	}
	if (VAL_TAG(text_val) != T_STRING) {
		fail(interp, "(parse-time): expected a string to parse; got %s", tag_name(VAL_TAG(text_val)));
		return;
	}

	Object *format = OBJECT_AT(VAL_DATA(format_val));
	Object *text = OBJECT_AT(VAL_DATA(text_val));
	struct tm calendar = {0};
	calendar.tm_year = 70;
	calendar.tm_mday = 1;
	if (!strptime(text->bytes, format->bytes, &calendar)) {
		fail(interp, "(parse-time): \"%s\" does not match \"%s\"", text->bytes, format->bytes);
		return;
	}

	long long days = days_from_civil(calendar.tm_year + 1900, calendar.tm_mon + 1, calendar.tm_mday);

	chain_sp[-2] = make_float((double)days * 86400.0 + calendar.tm_hour * 3600.0
			+ calendar.tm_min * 60.0 + calendar.tm_sec - (double)calendar.tm_gmtoff);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}
