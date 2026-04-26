-- Используем переменную :so_path из psql -v
\set ON_ERROR_STOP on

-- Тестовая схема
DROP SCHEMA IF EXISTS TEST_SCHEMA CASCADE;
CREATE SCHEMA TEST_SCHEMA;
SET search_path TO TEST_SCHEMA;

\echo '=== Loading functions from' :so_path '==='

-- Загружаем функцию напрямую из .so
CREATE FUNCTION value_to_jsonb(bytea)
RETURNS jsonb
AS :'so_path', 'value_to_jsonb'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION bytea_to_bigint(bytea) RETURNS bigint
AS :'so_path','bytea_to_bigint'
LANGUAGE c IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION bytea_to_numeric(bytea) RETURNS numeric
LANGUAGE 'c' STRICT IMMUTABLE
AS :'so_path', 'bytea_to_numeric';

CREATE OR REPLACE FUNCTION numeric_to_bytea(numeric,int DEFAULT 0) RETURNS bytea
LANGUAGE 'c' STRICT IMMUTABLE
AS :'so_path', 'numeric_to_bytea';

CREATE OR REPLACE FUNCTION bytea_to_str(bytea) RETURNS text
AS :'so_path','bytea_to_str'
LANGUAGE c IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION str_to_bytea(text) RETURNS bytea
AS :'so_path','str_to_bytea'
LANGUAGE c IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION bytea_to_bool(bytea) RETURNS bool
AS :'so_path','bytea_to_bool'
LANGUAGE c IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION bool_to_bytea(bool) RETURNS bytea
AS :'so_path','bool_to_bytea'
LANGUAGE c IMMUTABLE STRICT;

-- bytea_to_timestamp  → timestamp (no tz)  — для CAST (bytea AS timestamp)
CREATE OR REPLACE FUNCTION bytea_to_timestamp(bytea) RETURNS timestamp
AS :'so_path','bytea_to_timestamp'
LANGUAGE c IMMUTABLE STRICT;

-- bytea_to_timestamptz → timestamp with time zone — применяет tz offset из params
CREATE OR REPLACE FUNCTION bytea_to_timestamptz(bytea) RETURNS timestamp with time zone
AS :'so_path','bytea_to_timestamptz'
LANGUAGE c IMMUTABLE STRICT;

-- CAST: bytea → timestamp (no tz)
CREATE CAST (bytea AS timestamp) WITH FUNCTION bytea_to_timestamp(bytea);
-- CAST: bytea → timestamptz
CREATE CAST (bytea AS timestamptz) WITH FUNCTION bytea_to_timestamptz(bytea);

CREATE OR REPLACE FUNCTION timestamp_to_bytea(bigint,int DEFAULT 0) RETURNS bytea
AS :'so_path','timestamp_to_bytea'
LANGUAGE c IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION date_to_bytea(text,text,int DEFAULT 0) RETURNS bytea
AS :'so_path','date_to_bytea'
LANGUAGE c IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION bytea_to_hex(bytea) RETURNS text
AS :'so_path','bytea_to_hex'
LANGUAGE c IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION hex_to_bytea(text,int DEFAULT 0) RETURNS bytea
AS :'so_path','hex_to_bytea'
LANGUAGE c IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION uuid_to_bytea(uuid,int DEFAULT 0) RETURNS bytea
AS :'so_path','uuid_to_bytea'
LANGUAGE c IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION bytea_to_uuid(bytea) RETURNS uuid
AS :'so_path','bytea_to_uuid'
LANGUAGE c IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION jsonb_to_bytea(jsonb,int DEFAULT 0) RETURNS bytea
AS :'so_path','jsonb_to_bytea'
LANGUAGE c IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION bytea_to_jsonb(bytea) RETURNS jsonb
AS :'so_path','bytea_to_jsonb'
LANGUAGE c IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION bigint_to_bytea(bigint,int DEFAULT 0) RETURNS bytea
AS :'so_path','bigint_to_bytea'
LANGUAGE c IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION bytea_to_date(bytea) RETURNS timestamp
AS :'so_path','bytea_to_date'
LANGUAGE c IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION cmp_date(bytea,bytea) RETURNS int
AS :'so_path','cmp_date'
LANGUAGE c IMMUTABLE STRICT;

\echo '=== Running tests ==='
SELECT bytea_to_bigint('\x20002a') AS test_big;
SELECT bytea_to_numeric('\x20012a') AS test_num;

SELECT value_to_jsonb('\x20002a') AS test_int_json;
SELECT value_to_jsonb('\x106268656c6c6f20776f726c64') AS test_text_json;
SELECT value_to_jsonb('\x400069469848') AS test_date_json;
SELECT value_to_jsonb('\x2002013a') AS test_numeric_json;
SELECT value_to_jsonb('\x8000ac861c6452aeb2234d6a5c26fc34994c') AS test_numeric_json;
SELECT value_to_jsonb('\xf0004838b106fce9647bdf1e7877bf73ce8b0bad5f97') AS test_hex_json;
SELECT value_to_jsonb('\x40006947f42d') AS test_date_json;
SELECT value_to_jsonb('\xe00002000020030000800400000009000010050000006167656e616d65002000000000803800457667656e') AS test_json_json;

SELECT bool_to_bytea(true) AS test_bool_bytea;
SELECT bytea_to_bool(bool_to_bytea('t')) AS test_bool;
SELECT timestamp_to_bytea(1766323245) as test_time_to_bytea;
-- bytea_to_timestamp → plain timestamp (no tz), tz offset в params игнорируется
SELECT bytea_to_timestamp(timestamp_to_bytea(1766323245)) as test_timestamp_notz;
-- bytea_to_timestamptz → timestamp with time zone, tz offset применяется
SELECT bytea_to_timestamptz(timestamp_to_bytea(1766323245)) as test_timestamptz;
-- Roundtrip с tz offset +120 мин (UTC+2): bytea хранит offset, timestamptz его применяет
SELECT bytea_to_timestamptz(timestamp_to_bytea(1766323245, 120)) as test_timestamptz_tz_plus2;
-- CAST test (bytea AS timestamp)
SELECT CAST(timestamp_to_bytea(1766323245) AS timestamp) as test_cast_timestamp;
-- CAST test (bytea AS timestamptz)
SELECT CAST(timestamp_to_bytea(1766323245) AS timestamptz) as test_cast_timestamptz;
SELECT date_to_bytea(now()::text,'YYYY-MM-DD HH24:MI:SS.USTZH') as test_now_to_bytea;
SELECT bytea_to_hex('\xf0004838b106fce9647bdf1e7877bf73ce8b0bad5f97') as test_byte_to_hex;
SELECT hex_to_bytea('0x4838b106fce9647bdf1e7877bf73ce8b0bad5f97') as test_hex_to_byte;
SELECT uuid_to_bytea('ac861c64-52ae-b223-4d6a-5c26fc34994c') as test_uuid_byte;
SELECT bytea_to_uuid('\x8000ac861c6452aeb2234d6a5c26fc34994c') as test_byte_uuid;
SELECT jsonb_to_bytea('{"name":"Evgen","age":56}') as test_jsonb_bytea;
SELECT bytea_to_jsonb(jsonb_to_bytea('{"name":"Evgen","age":56}')) as test_bytea_jsonb;
SELECT bigint_to_bytea(12345) as test_bigint_bytea;
SELECT bytea_to_date('\x40006949e08a') as test_bytea_date;
SELECT cmp_date('\x40006949e08a','\x40006949e08a') as test_cmp_date;
SELECT numeric_to_bytea(202.20::numeric) as test_numeric_to_byte;

-- ============================================================
-- str_to_bytea / bytea_to_str
-- ============================================================
\echo '--- str_to_bytea / bytea_to_str ---'

-- 1. Проверяем заголовок: первые 2 байта = \x10\x00 (op_id=0x01<<4, params=0)
--    Для 'hello' (5 байт) ожидаем: \x100068656c6c6f
SELECT str_to_bytea('hello') AS test_str_header;

-- 2. Roundtrip: короткая ASCII строка
SELECT bytea_to_str(str_to_bytea('hello world')) AS test_str_roundtrip_ascii;
-- Ожидаем: 'hello world'

-- 3. Roundtrip: строка длиннее 8 байт (bug #3: buf partial copy fix)
SELECT bytea_to_str(str_to_bytea('this is a longer string')) AS test_str_roundtrip_long;
-- Ожидаем: 'this is a longer string'

-- 4. Roundtrip: UTF-8 / кириллица (multibyte)
SELECT bytea_to_str(str_to_bytea('Привет мир')) AS test_str_roundtrip_utf8;
-- Ожидаем: 'Привет мир'

-- 5. Пустая строка
SELECT bytea_to_str(str_to_bytea('')) AS test_str_roundtrip_empty;
-- Ожидаем: ''


-- 6. value_to_jsonb + str_to_bytea: params=0 → весь payload
--    (bug fix: раньше params=0 давал copy_bytes=0 → пустую строку)
SELECT value_to_jsonb(str_to_bytea('hello world')) AS test_str_as_jsonb;
-- Ожидаем: "hello world"

-- 7. value_to_jsonb: UTF-8, params=0
SELECT value_to_jsonb(str_to_bytea('Привет')) AS test_str_utf8_as_jsonb;
-- Ожидаем: "Привет"

-- 8. Старый формат: params=0x62=98 (length hint) > data_len=11 → весь текст
SELECT value_to_jsonb('\x106268656c6c6f20776f726c64') AS test_text_json_params_hint;
-- Ожидаем: "hello world"

-- 9. Новый формат str_to_bytea: params=0x00 → весь payload (13 байт строки)
SELECT value_to_jsonb(str_to_bytea('hello world')) AS test_text_json_params_zero;
-- Ожидаем: "hello world"

-- ============================================================

-- ============================================================
-- Validation / anti-crash tests (pg_ilib_check_header)
-- ============================================================
\echo '--- Validation: невозможные params должны дать ERROR ---'

-- numeric: scale=0xFFF=4095 при payload=1 байт → невозможно (max scale=3)
-- Ожидаем ERROR: numeric scale 4095 is impossible for 1 payload byte(s)
DO $$
BEGIN
  PERFORM bytea_to_numeric('\x2F002a'::bytea);  -- op_id=0x02, params=0xF00 -> scale=3840
  RAISE EXCEPTION 'TEST FAILED: should have raised DATA_CORRUPTED';
EXCEPTION WHEN others THEN
  RAISE NOTICE 'OK: bytea_to_numeric rejected impossible scale: %', SQLERRM;
END $$;

-- timestamp: tz offset=0xFFF=4095 минут → вне диапазона [-840,840]
-- Ожидаем ERROR: timezone offset 4095 min is out of valid range
DO $$
BEGIN
  PERFORM bytea_to_timestamp('\x4FFF12345678'::bytea);
  RAISE EXCEPTION 'TEST FAILED: should have raised DATA_CORRUPTED';
EXCEPTION WHEN others THEN
  RAISE NOTICE 'OK: bytea_to_timestamp rejected tz offset 0xFFF: %', SQLERRM;
END $$;

-- bool: неверный размер payload (передаём 4 байта вместо 1)
-- Ожидаем ERROR: bool payload must be 1 byte
DO $$
BEGIN
  PERFORM bytea_to_bool('\x300012345678'::bytea);  -- op_id=0x03, 4 payload bytes
  RAISE EXCEPTION 'TEST FAILED: should have raised DATA_CORRUPTED';
EXCEPTION WHEN others THEN
  RAISE NOTICE 'OK: bytea_to_bool rejected wrong payload size: %', SQLERRM;
END $$;

-- uuid: неверный размер payload (слишком короткий)
-- Ожидаем NULL (существующая проверка) либо ERROR
DO $$
BEGIN
  PERFORM bytea_to_uuid('\x80001234'::bytea);  -- op_id=0x08, только 2 payload bytes
  RAISE NOTICE 'OK: bytea_to_uuid returned NULL for short uuid payload';
EXCEPTION WHEN others THEN
  RAISE NOTICE 'OK: bytea_to_uuid raised error for short uuid payload: %', SQLERRM;
END $$;

-- value_to_jsonb: неизвестный op_id=0x09 → ERROR
DO $$
BEGIN
  PERFORM value_to_jsonb('\x90001234'::bytea);  -- op_id=0x09 unknown
  RAISE EXCEPTION 'TEST FAILED: should have raised DATA_CORRUPTED';
EXCEPTION WHEN others THEN
  RAISE NOTICE 'OK: value_to_jsonb rejected unknown op_id 0x09: %', SQLERRM;
END $$;

-- value_to_jsonb: слишком короткий bytea (1 байт)
DO $$
BEGIN
  PERFORM value_to_jsonb('\x20'::bytea);  -- только 1 байт, нет params
  RAISE EXCEPTION 'TEST FAILED: should have raised DATA_CORRUPTED';
EXCEPTION WHEN others THEN
  RAISE NOTICE 'OK: value_to_jsonb rejected too-short bytea: %', SQLERRM;
END $$;
-- ============================================================

\echo '=== Cleanup ==='
DROP SCHEMA TEST_SCHEMA CASCADE;

\echo '=== Tests completed! ==='
