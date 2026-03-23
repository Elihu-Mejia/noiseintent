CC = gcc
CFLAGS = -O2 -fPIC -Wall -Wextra
LUA_VERSION = 5.5

# Detect OS
UNAME_S := $(shell uname -s)

# Attempt to use pkg-config for Lua, fall back to standard naming if not found
LUA_CFLAGS := $(shell pkg-config --cflags lua$(LUA_VERSION) 2>/dev/null)
LUA_LIBS := $(shell pkg-config --libs lua$(LUA_VERSION) 2>/dev/null)

# Fallback include paths if pkg-config fails
ifeq ($(strip $(LUA_CFLAGS)),)
	LUA_CFLAGS = -I/usr/include/lua$(LUA_VERSION) -I/usr/local/include/lua$(LUA_VERSION)
	# macOS Apple Silicon Homebrew path check
	ifeq ($(UNAME_S),Darwin)
		ifneq ($(wildcard /opt/homebrew/include/lua$(LUA_VERSION)),)
			LUA_CFLAGS = -I/opt/homebrew/include/lua$(LUA_VERSION)
		endif
	endif
endif

# OS-Specific Settings
ifeq ($(UNAME_S),Darwin)
	LDFLAGS = -bundle -undefined dynamic_lookup
	LIBS = $(LUA_LIBS) -lpthread -lm -framework AudioToolbox -framework CoreAudio
else
	LDFLAGS = -shared
	LIBS = $(LUA_LIBS) -lasound -lpthread -lm
endif

TARGET = noiseintent.so
SRC = noiseintent.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LUA_CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
