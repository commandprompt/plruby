--
-- Date/time types: date, timestamp, timestamptz, time, interval.  All cross
-- the string conversion path in both directions.  Pin the session formats so
-- the output is stable regardless of server defaults.
--
SET datestyle = 'ISO, MDY';
SET timezone = 'UTC';
SET intervalstyle = 'postgres';

-- All five arrive as Ruby Strings.
CREATE FUNCTION dt_classes(date, timestamp, timestamptz, time, interval)
RETURNS text LANGUAGE plruby AS $$
    args.map { |a| a.class.name }.uniq.join(' ')
$$;
SELECT dt_classes('2026-07-05', '2026-07-05 12:34:56',
                  '2026-07-05 12:34:56+02', '12:34:56', '1 day 02:03:04');

-- The text forms use the session's DateStyle/TimeZone (ISO, UTC here).
CREATE FUNCTION dt_echo(date, timestamp, timestamptz, time, interval)
RETURNS text LANGUAGE plruby AS $$
    args.join(' | ')
$$;
SELECT dt_echo('2026-07-05', '2026-07-05 12:34:56',
               '2026-07-05 12:34:56+02', '12:34:56', '1 day 02:03:04');

-- Returning a string converts back through the type's input function.
CREATE FUNCTION dt_make_date() RETURNS date LANGUAGE plruby AS $$
    '2026-07-05'
$$;
SELECT dt_make_date(), dt_make_date() + 7 AS week_later;

-- Ruby's own date library interoperates: parse, add, return.
CREATE FUNCTION dt_add_days(date, int) RETURNS date LANGUAGE plruby AS $$
    require 'date'
    (Date.parse(args[0]) + args[1]).iso8601
$$;
SELECT dt_add_days('2026-07-05', 30);

-- Building a timestamp from Ruby Time (a fixed instant; UTC in and out).
CREATE FUNCTION dt_fixed_ts() RETURNS timestamptz LANGUAGE plruby AS $$
    Time.utc(2026, 7, 5, 12, 0, 0).strftime('%Y-%m-%d %H:%M:%S+00')
$$;
SELECT dt_fixed_ts();

-- Intervals are strings too; arithmetic happens in SQL or via parsing.
CREATE FUNCTION dt_interval_echo(interval) RETURNS interval LANGUAGE plruby AS $$
    args[0]
$$;
SELECT dt_interval_echo('90 minutes');

-- NULL dates arrive as nil like any other type.
CREATE FUNCTION dt_null(date) RETURNS text LANGUAGE plruby AS $$
    args[0].nil? ? 'nil' : args[0]
$$;
SELECT dt_null(NULL);

-- An invalid string returned into a date is rejected by the input function.
CREATE FUNCTION dt_bad() RETURNS date LANGUAGE plruby AS $$
    'not-a-date'
$$;
SELECT dt_bad();

RESET datestyle;
RESET timezone;
RESET intervalstyle;
