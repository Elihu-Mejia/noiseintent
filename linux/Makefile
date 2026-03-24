CC = gcc
CFLAGS = -O2 -fPIC -Wall -Wextra
LUA_VERSION ?= 5.4

# Attempt to use pkg-config for Lua, fall back to standard naming if not found
# Iterate through common package names: lua5.4, lua-5.4, lua
LUA_PC_NAMES = lua$(LUA_VERSION) lua-$(LUA_VERSION) lua
LUA_CFLAGS := $(shell for p in $(LUA_PC_NAMES); do pkg-config --cflags $$p 2>/dev/null && break; done)
LUA_LIBS := $(shell for p in $(LUA_PC_NAMES); do pkg-config --libs $$p 2>/dev/null && break; done)

# Fallback include paths if pkg-config fails
ifeq ($(strip $(LUA_CFLAGS)),)
	LUA_CFLAGS = -I/usr/include/lua$(LUA_VERSION) -I/usr/local/include/lua$(LUA_VERSION)
endif

LDFLAGS = -shared
LIBS = $(LUA_LIBS) -lasound -lpthread -lm

TARGET = noiseintent.so
SRC = noiseintent.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LUA_CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
