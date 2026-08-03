-- pg_ilib--1.5.sql
-- Full installation of pg_ilib v1.5
--
-- pg_ilib: typed binary serialization for PostgreSQL
--
-- Binary format:  2-byte header  [ op_id(4 bits) | params(12 bits) ]
--                 followed by payload bytes
--
-- op_id map:
--   0x01  text (str)
--   0x02  numeric / bigint
--   0x03  bool
--   0x04  timestamp / date
--   0x08  uuid
--   0x0E  jsonb
--   0x0F  hex bytes
--
-- Functions removed in 1.5 (now in separate extensions):
--   tmpl_render      → pg_tmpl
--   ws_send_request  → pg_curl
--   telegram_*       → pg_curl

-- ── text ─────────────────────────────────────────────────────────────────

CREATE FUNCTION str_to_bytea(text)
    RETURNS bytea
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'str_to_bytea';

CREATE FUNCTION bytea_to_str(bytea)
    RETURNS text
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'bytea_to_str';

-- ── bigint ────────────────────────────────────────────────────────────────

CREATE FUNCTION bigint_to_bytea(bigint, int DEFAULT 0)
    RETURNS bytea
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'bigint_to_bytea';

CREATE FUNCTION bytea_to_bigint(bytea)
    RETURNS bigint
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'bytea_to_bigint';

CREATE CAST (bigint AS bytea) WITH FUNCTION bigint_to_bytea(bigint,int);
CREATE CAST (bytea AS bigint) WITH FUNCTION bytea_to_bigint(bytea);

-- ── numeric ───────────────────────────────────────────────────────────────

CREATE FUNCTION numeric_to_bytea(numeric, int DEFAULT 0)
    RETURNS bytea
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'numeric_to_bytea';

CREATE FUNCTION bytea_to_numeric(bytea)
    RETURNS numeric
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'bytea_to_numeric';

CREATE CAST (numeric AS bytea) WITH FUNCTION numeric_to_bytea(numeric, int);
CREATE CAST (bytea AS numeric) WITH FUNCTION bytea_to_numeric(bytea);

-- ── bool ──────────────────────────────────────────────────────────────────

CREATE FUNCTION bool_to_bytea(bool)
    RETURNS bytea
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'bool_to_bytea';

CREATE FUNCTION bytea_to_bool(bytea)
    RETURNS bool
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'bytea_to_bool';

CREATE CAST (bool AS bytea) WITH FUNCTION bool_to_bytea(bool);
CREATE CAST (bytea AS bool) WITH FUNCTION bytea_to_bool(bytea);

-- ── timestamp ─────────────────────────────────────────────────────────────

-- timestamp_to_bytea(unix_seconds bigint, tz_offset_minutes int DEFAULT 0)
CREATE FUNCTION timestamp_to_bytea(bigint, int DEFAULT 0)
    RETURNS bytea
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'timestamp_to_bytea';

-- bytea_to_timestamp → plain timestamp (no tz); tz offset in params ignored
CREATE FUNCTION bytea_to_timestamp(bytea)
    RETURNS timestamp
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'bytea_to_timestamp';

-- bytea_to_timestamptz → applies stored tz offset from params
CREATE FUNCTION bytea_to_timestamptz(bytea)
    RETURNS timestamp with time zone
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'bytea_to_timestamptz';

CREATE CAST (bytea AS timestamp)   WITH FUNCTION bytea_to_timestamp(bytea);
CREATE CAST (bytea AS timestamptz) WITH FUNCTION bytea_to_timestamptz(bytea);

-- ── date ──────────────────────────────────────────────────────────────────

CREATE FUNCTION date_to_bytea(text, text, int DEFAULT 0)
    RETURNS bytea
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'date_to_bytea';

CREATE FUNCTION bytea_to_date(bytea)
    RETURNS timestamp
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'bytea_to_date';

-- cmp_date: returns -1, 0, 1  (left < right, equal, left > right)
CREATE FUNCTION cmp_date(bytea, bytea)
    RETURNS int
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'cmp_date';

-- ── uuid ──────────────────────────────────────────────────────────────────

CREATE FUNCTION uuid_to_bytea(uuid, int DEFAULT 0)
    RETURNS bytea
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'uuid_to_bytea';

CREATE FUNCTION bytea_to_uuid(bytea)
    RETURNS uuid
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'bytea_to_uuid';

CREATE CAST (uuid AS bytea) WITH FUNCTION uuid_to_bytea(uuid,int);
CREATE CAST (bytea AS uuid) WITH FUNCTION bytea_to_uuid(bytea);

-- ── hex ───────────────────────────────────────────────────────────────────

CREATE FUNCTION hex_to_bytea(text, int DEFAULT 0)
    RETURNS bytea
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'hex_to_bytea';

CREATE FUNCTION bytea_to_hex(bytea)
    RETURNS text
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'bytea_to_hex';

-- ── jsonb ─────────────────────────────────────────────────────────────────

CREATE FUNCTION jsonb_to_bytea(jsonb, int DEFAULT 0)
    RETURNS bytea
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'jsonb_to_bytea';

CREATE FUNCTION bytea_to_jsonb(bytea)
    RETURNS jsonb
    LANGUAGE c IMMUTABLE STRICT
    AS '$libdir/pg_ilib', 'bytea_to_jsonb';

CREATE CAST (jsonb AS bytea) WITH FUNCTION jsonb_to_bytea(jsonb,int);
CREATE CAST (bytea AS jsonb) WITH FUNCTION bytea_to_jsonb(bytea);

-- ── universal decoder ─────────────────────────────────────────────────────

-- value_to_jsonb: reads op_id from header and returns appropriate JSON type
CREATE FUNCTION value_to_jsonb(bytea)
    RETURNS jsonb
    LANGUAGE c STRICT
    AS '$libdir/pg_ilib', 'value_to_jsonb';
