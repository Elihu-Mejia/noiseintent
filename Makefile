CC = gcc
CFLAGS = -O2 -fPIC -Wall -Wextra
LDFLAGS = -shared
LUA_VERSION = 5.5

# Attempt to use pkg-config for Lua, fall back to standard naming if not found
LUA_CFLAGS := $(shell pkg-config --cflags lua$(LUA_VERSION) 2>/dev/null)
LUA_LIBS := $(shell pkg-config --libs lua$(LUA_VERSION) 2>/dev/null)

ifeq ($(strip $(LUA_CFLAGS)),)
	LUA_CFLAGS = -I/usr/include/lua$(LUA_VERSION) -I/usr/local/include/lua$(LUA_VERSION)
endif

LIBS = $(LUA_LIBS) -lasound -lpthread -lm

TARGET = noiseintent.so
SRC = noiseintent.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LUA_CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
