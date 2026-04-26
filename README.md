# pg_ilib

A PostgreSQL C extension for compact typed binary serialization.

Values of different SQL types are encoded into `bytea` with a 2-byte header that carries the type identifier (`op_id`) and optional metadata (`params`). The universal decoder `value_to_jsonb()` can read any encoded value without knowing its type in advance — useful for schemaless or dynamic-column storage patterns.

## Binary format

```
Byte 0: [ op_id (4 bits) | params_hi (4 bits) ]
Byte 1: [ params_lo (8 bits) ]
Bytes 2…N: payload
```

| op_id | Type | params meaning | Payload |
|-------|------|----------------|---------|
| `0x01` | text | — | UTF-8 bytes |
| `0x02` | numeric / bigint | decimal scale | GMP big-endian integer |
| `0x03` | bool | — | 1 byte: `0x01` true, `0x00` false |
| `0x04` | timestamp / date | tz offset in minutes (signed) | 4-byte Unix timestamp (big-endian) |
| `0x08` | uuid | — | 16 raw UUID bytes |
| `0x0E` | jsonb | — | PostgreSQL binary JSONB |
| `0x0F` | hex | — | raw bytes (displayed as `0x…`) |

All decoders validate the header before touching payload data and raise `ERRCODE_DATA_CORRUPTED` on impossible combinations, so malformed input cannot crash the server.

## Requirements

- PostgreSQL 11 or later
- [GNU MP (GMP)](https://gmplib.org/) — `libgmp-dev` on Debian/Ubuntu, `gmp-devel` on RHEL/CentOS

## Build & install

```bash
make
sudo make install

# In psql:
CREATE EXTENSION pg_ilib;
```

To upgrade from v1.4:

```sql
ALTER EXTENSION pg_ilib UPDATE TO '1.5';
```

> **Note:** v1.5 removes `tmpl_render`, `ws_send_request`, and `telegram_*` functions.
> These now live in the separate `pg_tmpl` and `pg_curl` extensions in this same repository.

## Quick test (no install required)

```bash
make testdb        # create pg_ilib_test database (once)
make quicktest     # build and run test/quick_test.sql against a temp .so
```

## Functions

### text

```sql
str_to_bytea(text)          → bytea
bytea_to_str(bytea)         → text
```

```sql
SELECT bytea_to_str(str_to_bytea('hello world'));
-- hello world

SELECT str_to_bytea('hello');
-- \x100068656c6c6f
--  ^^-- op_id=0x01, params=0x000
```

### bigint

```sql
bigint_to_bytea(bigint, scale int DEFAULT 0)  → bytea
bytea_to_bigint(bytea)                        → bigint
```

```sql
SELECT bytea_to_bigint(bigint_to_bytea(123456789));
-- 123456789

-- implicit CAST
SELECT 42::bigint::bytea;
SELECT '\x200000000000002a'::bytea::bigint;
```

### numeric

```sql
numeric_to_bytea(numeric, scale int DEFAULT 0)  → bytea
bytea_to_numeric(bytea)                         → numeric
```

`scale` is the number of decimal places stored in `params` and used during decoding.

```sql
SELECT bytea_to_numeric(numeric_to_bytea(3.14159, 5));
-- 3.14159

SELECT bytea_to_numeric(numeric_to_bytea(202.20, 2));
-- 202.20
```

### bool

```sql
bool_to_bytea(bool)   → bytea
bytea_to_bool(bytea)  → bool
```

```sql
SELECT bytea_to_bool(bool_to_bytea(true));
-- t
```

### timestamp

```sql
-- Encode: unix_seconds bigint, tz_offset_minutes int DEFAULT 0
timestamp_to_bytea(bigint, int DEFAULT 0)  → bytea

-- Decode as plain timestamp (tz offset in params is ignored)
bytea_to_timestamp(bytea)    → timestamp

-- Decode as timestamptz (tz offset from params is applied)
bytea_to_timestamptz(bytea)  → timestamptz
```

```sql
-- Store current time (UTC)
SELECT timestamp_to_bytea(extract(epoch from now())::bigint);

-- Roundtrip
SELECT bytea_to_timestamp(timestamp_to_bytea(1766323245));
-- 2026-01-21 00:00:45

-- With UTC+2 offset
SELECT bytea_to_timestamptz(timestamp_to_bytea(1766323245, 120));
-- 2026-01-21 02:00:45+02

-- Implicit CASTs
SELECT timestamp_to_bytea(1766323245)::timestamp;
SELECT timestamp_to_bytea(1766323245)::timestamptz;
```

### date

```sql
date_to_bytea(date_str text, format text, tz_offset int DEFAULT 0)  → bytea
bytea_to_date(bytea)                                                  → timestamp
cmp_date(bytea, bytea)                                                → int   -- -1 / 0 / 1
```

```sql
SELECT date_to_bytea(now()::text, 'YYYY-MM-DD HH24:MI:SS.USTZH');
SELECT bytea_to_date('\x40006949e08a');

-- Compare two encoded dates
SELECT cmp_date(
    date_to_bytea('2024-01-01', 'YYYY-MM-DD'),
    date_to_bytea('2024-06-01', 'YYYY-MM-DD')
);  -- -1  (first is earlier)
```

### uuid

```sql
uuid_to_bytea(uuid, int DEFAULT 0)  → bytea
bytea_to_uuid(bytea)                → uuid
```

```sql
SELECT bytea_to_uuid(uuid_to_bytea('ac861c64-52ae-b223-4d6a-5c26fc34994c'));
-- ac861c64-52ae-b223-4d6a-5c26fc34994c

-- Implicit CASTs
SELECT 'ac861c64-52ae-b223-4d6a-5c26fc34994c'::uuid::bytea;
```

### hex

```sql
hex_to_bytea(hex_str text, int DEFAULT 0)  → bytea   -- input: "0x4838b1…"
bytea_to_hex(bytea)                        → text    -- output: "0x4838b1…"
```

```sql
SELECT bytea_to_hex(hex_to_bytea('0x4838b106fce9647bdf1e7877bf73ce8b0bad5f97'));
-- 0x4838b106fce9647bdf1e7877bf73ce8b0bad5f97
```

### jsonb

```sql
jsonb_to_bytea(jsonb, int DEFAULT 0)  → bytea
bytea_to_jsonb(bytea)                 → jsonb
```

```sql
SELECT bytea_to_jsonb(jsonb_to_bytea('{"name":"Alice","age":30}'::jsonb));
-- {"age": 30, "name": "Alice"}
```

### Universal decoder

```sql
value_to_jsonb(bytea)  → jsonb
```

Reads `op_id` from the header and converts the payload to an appropriate JSON value. Useful when you store mixed types in a single column.

```sql
SELECT value_to_jsonb(bigint_to_bytea(42));        -- 42
SELECT value_to_jsonb(str_to_bytea('hello'));       -- "hello"
SELECT value_to_jsonb(bool_to_bytea(true));         -- true
SELECT value_to_jsonb(uuid_to_bytea('ac861c64-52ae-b223-4d6a-5c26fc34994c'));
-- "ac861c64-52ae-b223-4d6a-5c26fc34994c"
SELECT value_to_jsonb(numeric_to_bytea(3.14, 2));   -- 3.14
```

## Error handling

All decoders reject invalid input with a descriptive error:

```sql
-- Scale impossible for payload size
SELECT bytea_to_numeric('\x2FFF2a');
-- ERROR: pg_ilib bytea_to_numeric: numeric scale 4095 is impossible
--        for 1 payload byte(s) (max scale = 3)

-- Timezone offset out of range
SELECT bytea_to_timestamp('\x4FFF12345678');
-- ERROR: pg_ilib bytea_to_timestamp[tz]: timezone offset 4095 min
--        is out of valid range [-840, 840]

-- Unknown type
SELECT value_to_jsonb('\x9000ff');
-- ERROR: pg_ilib value_to_jsonb: unknown op_id 0x09
```

## Related extensions (same repository)

| Extension | Description |
|-----------|-------------|
| `pg_tmpl` | Mustache template rendering (`tmpl_render`) |
| `pg_curl` | HTTP / WebSocket / Telegram Bot API |

Build each independently:

```bash
make -f Makefile.tmpl && sudo make -f Makefile.tmpl install
make -f Makefile.curl && sudo make -f Makefile.curl install
```

Or build all three at once:

```bash
make all-ext && sudo make install-ext
```

## License

MIT
