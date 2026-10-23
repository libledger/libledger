# Makefile for libledger / ledger CLI
#
# Works on Linux, macOS (including the stock GNU Make 3.81) and
# Windows (MSYS2 / MinGW-w64).
#
# Dependencies: SQLite3 and cJSON.
#  - If the development packages are installed (pkg-config finds
#    "sqlite3" and "libcjson"), the system libraries are used.
#  - Otherwise the sources (SQLite amalgamation and cJSON) are
#    downloaded into deps/ and compiled into the program.
#    Force this mode with: make USE_SYSTEM_LIBS=0

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra
LDLIBS  += -lm

ifeq ($(OS),Windows_NT)
  EXE      := .exe
  ifeq ($(origin CC),default)
    CC := gcc
  endif
  # MinGW has no getline(); provide it without touching the sources.
  CPPFLAGS += -Icompat -include compat/getline_compat.h
  COMPAT_OBJS := compat/getline_compat.o
  SYS_LIBS :=
else
  EXE      :=
  COMPAT_OBJS :=
  SYS_LIBS := -lpthread -ldl
endif

PROG    := ledger$(EXE)
LIB     := libledger.a

SQLITE_VERSION := 3460100
SQLITE_YEAR    := 2024
SQLITE_DIR     := deps/sqlite-amalgamation-$(SQLITE_VERSION)
SQLITE_ZIP     := deps/sqlite-amalgamation-$(SQLITE_VERSION).zip
SQLITE_URL     := https://www.sqlite.org/$(SQLITE_YEAR)/sqlite-amalgamation-$(SQLITE_VERSION).zip

CJSON_VERSION  := 1.7.19
CJSON_DIR      := deps/cJSON-$(CJSON_VERSION)
CJSON_URL      := https://raw.githubusercontent.com/DaveGamble/cJSON/v$(CJSON_VERSION)

DOWNLOAD := $(shell command -v curl >/dev/null 2>&1 && echo "curl -fsSL -o" || echo "wget -q -O")

ifndef USE_SYSTEM_LIBS
USE_SYSTEM_LIBS := $(shell pkg-config --exists sqlite3 libcjson 2>/dev/null && echo 1 || echo 0)
endif

ifeq ($(USE_SYSTEM_LIBS),1)
  # libcjson's pkg-config cflags point to .../include/cjson, so
  # #include "cJSON.h" works as-is.
  CPPFLAGS += $(shell pkg-config --cflags sqlite3 libcjson)
  LDLIBS   := $(shell pkg-config --libs sqlite3 libcjson) $(LDLIBS)
  DEP_OBJS :=
  DEP_HDRS :=
else
  CPPFLAGS += -I$(SQLITE_DIR) -I$(CJSON_DIR)
  LDLIBS   += $(SYS_LIBS)
  DEP_OBJS := $(SQLITE_DIR)/sqlite3.o $(CJSON_DIR)/cJSON.o
  DEP_HDRS := $(SQLITE_DIR)/sqlite3.h $(CJSON_DIR)/cJSON.h
endif

.PHONY: all clean distclean deps

all: $(PROG)

$(PROG): ledger_cli.o $(LIB) $(DEP_OBJS) $(COMPAT_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ ledger_cli.o $(LIB) $(DEP_OBJS) $(COMPAT_OBJS) $(LDLIBS)

$(LIB): ledger_impl.o
	$(AR) rcs $@ $^

%.o: %.c ledger.h $(DEP_HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

compat/getline_compat.o: compat/getline_compat.c compat/getline_compat.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- bundled dependencies -------------------------------------------

deps: $(DEP_HDRS)

$(SQLITE_ZIP):
	@mkdir -p deps
	$(DOWNLOAD) $@ $(SQLITE_URL)

# Unzipping yields both sqlite3.h and sqlite3.c; the header is the
# rule's target and the .c just rides along.
$(SQLITE_DIR)/sqlite3.h: $(SQLITE_ZIP)
	cd deps && unzip -oq $(notdir $(SQLITE_ZIP))
	@touch $(SQLITE_DIR)/sqlite3.c $(SQLITE_DIR)/sqlite3.h

$(SQLITE_DIR)/sqlite3.c: $(SQLITE_DIR)/sqlite3.h

$(SQLITE_DIR)/sqlite3.o: $(SQLITE_DIR)/sqlite3.c $(SQLITE_DIR)/sqlite3.h
	$(CC) -O2 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -c -o $@ $<

$(CJSON_DIR)/cJSON.c $(CJSON_DIR)/cJSON.h:
	@mkdir -p $(CJSON_DIR)
	$(DOWNLOAD) $@ $(CJSON_URL)/$(notdir $@)

$(CJSON_DIR)/cJSON.o: $(CJSON_DIR)/cJSON.c $(CJSON_DIR)/cJSON.h
	$(CC) -O2 -c -o $@ $<

# ---- cleanup --------------------------------------------------------

clean:
	rm -f ledger ledger.exe $(LIB) *.o compat/*.o $(SQLITE_DIR)/*.o $(CJSON_DIR)/*.o

distclean: clean
	rm -rf deps
