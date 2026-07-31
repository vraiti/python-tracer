PREFIX ?= /opt/trace-python
CPYTHON_DIR := cpython
CPYTHON_BIN := $(CPYTHON_DIR)/python
NPROC := $(shell nproc)

CC ?= gcc

CPYTHON_INC := $(CPYTHON_DIR)/Include
CPYTHON_INTERNAL_INC := $(CPYTHON_DIR)/Include/internal
CPYTHON_ROOT := $(CPYTHON_DIR)
EXT_SUFFIX := $(shell $(CPYTHON_BIN) -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX'))")

TRACER_SO := tracer/_tracer$(EXT_SUFFIX)
TRACER_SRCS := $(wildcard tracer/csrc/*.c)
TRACER_HDRS := $(wildcard tracer/csrc/*.h)

.PHONY: all cpython install clean ext

all: ext

ext: $(TRACER_SO)

$(TRACER_SO): $(TRACER_SRCS) $(TRACER_HDRS)
	$(CC) -O2 -shared -fPIC \
		-I$(CPYTHON_INC) \
		-I$(CPYTHON_INTERNAL_INC) \
		-I$(CPYTHON_ROOT) \
		-Itracer/csrc \
		$(TRACER_SRCS) -o $@

CPYTHON_SRCS := $(shell find $(CPYTHON_DIR) -name '*.c' -o -name '*.h' -o -name '*.py' 2>/dev/null | grep -v __pycache__)
CPYTHON_STAMP := .cpython-install.stamp

cpython: $(CPYTHON_BIN)

$(CPYTHON_BIN):
	cd $(CPYTHON_DIR) && CONFIG_SHELL=/bin/sh ./configure --prefix=$(PREFIX) && $(MAKE) -j$(NPROC)

$(CPYTHON_STAMP): $(CPYTHON_SRCS) $(CPYTHON_BIN)
	cd $(CPYTHON_DIR) && $(MAKE) -j$(NPROC) && $(MAKE) install
	touch $@

install: $(CPYTHON_STAMP) ext
	$(PREFIX)/bin/python3 -m pip install -e .

clean:
	cd $(CPYTHON_DIR) && $(MAKE) clean 2>/dev/null || true
	rm -f $(TRACER_SO)
