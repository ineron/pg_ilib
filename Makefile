# ── pg_ilib: typed bytea serialization ───────────────────────────────────
MODULE_big = pg_ilib
EXTENSION  = pg_ilib
EXTVERSION = 1.5

DATA = pg_ilib--1.5.sql

OBJS = pg_ilib.o

# Disable LLVM JIT compilation for this extension
override with_llvm = no

# Let pg_config find the right include path for the installed PostgreSQL
PG_CONFIG  = pg_config
PGXS      := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# GMP is required for numeric/bigint serialization
SHLIB_LINK := -lgmp

# ── all three extensions ──────────────────────────────────────────────────
# Build/install/clean pg_ilib + pg_tmpl + pg_curl in one shot.
#
#   make all-ext
#   sudo make install-ext

.PHONY: all-ext install-ext clean-ext

all-ext:
	$(MAKE)
	$(MAKE) -f Makefile.tmpl
	$(MAKE) -f Makefile.curl

install-ext:
	$(MAKE) install
	$(MAKE) -f Makefile.tmpl install
	$(MAKE) -f Makefile.curl install

clean-ext:
	$(MAKE) clean
	$(MAKE) -f Makefile.tmpl clean
	$(MAKE) -f Makefile.curl clean

# ── testing (no install required) ────────────────────────────────────────
# Usage:
#   make testdb      — create the test database once
#   make quicktest   — build and run tests against a temp .so copy
#
# Override host/user if needed:
#   make quicktest PG_HOST=10.0.0.1 PG_USER=myuser

TEST_SO_DIR  = /tmp/pg_ilib_test
TEST_SO_FILE = $(TEST_SO_DIR)/pg_ilib.so
TEST_DB      = pg_ilib_test
PG_HOST      ?= 127.0.0.1
PG_USER      ?= postgres

.PHONY: testdb quicktest rebuild-test

testdb:
	@echo "Creating test database $(TEST_DB)..."
	-dropdb  -h $(PG_HOST) -U $(PG_USER) $(TEST_DB) 2>/dev/null || true
	createdb -h $(PG_HOST) -U $(PG_USER) $(TEST_DB)
	@echo "Done. Run 'make quicktest' to run the tests."

quicktest: all
	@mkdir -p $(TEST_SO_DIR)
	@cp pg_ilib.so $(TEST_SO_FILE)
	@chmod 755 $(TEST_SO_FILE)
	@echo "=== Running tests ($(TEST_SO_FILE)) ==="
	@psql -h $(PG_HOST) -U $(PG_USER) -d $(TEST_DB) \
	      -v so_path=$(TEST_SO_FILE) \
	      -f test/quick_test.sql

rebuild-test: clean quicktest

# ── production install (with confirmation) ───────────────────────────────
.PHONY: install-prod

install-prod:
	@echo "WARNING: this will overwrite the installed pg_ilib."
	@read -p "Continue? [y/N] " -r; \
	echo; \
	if [[ $$REPLY =~ ^[Yy]$$ ]]; then \
	    $(MAKE) install; \
	    echo "Installed."; \
	else \
	    echo "Cancelled."; \
	fi
