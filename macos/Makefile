CC = gcc

# MacOS specific flags for Lua C modules:
# -bundle: Create a loadable bundle instead of a shared library
# -undefined dynamic_lookup: Don't link against Lua library, expect symbols from host
CFLAGS = -O2 -Wall -fPIC
LDFLAGS = -bundle -undefined dynamic_lookup -framework AudioToolbox -framework CoreAudio

# Include paths for Lua headers. 
# Adjust if using a specific version (e.g., /usr/local/include/lua5.4)
LUA_INC_INTEL ?= /usr/local/include
LUA_INC_ARM ?= /opt/homebrew/include

# Add common Lua include paths (Homebrew often nests headers in versioned folders)
INCLUDES = -I. \
	-I$(LUA_INC_INTEL) -I$(LUA_INC_INTEL)/lua -I$(LUA_INC_INTEL)/lua5.4 -I$(LUA_INC_INTEL)/lua5.3 -I$(LUA_INC_INTEL)/luajit-2.1 \
	-I$(LUA_INC_ARM) -I$(LUA_INC_ARM)/lua -I$(LUA_INC_ARM)/lua5.4 -I$(LUA_INC_ARM)/lua5.3 -I$(LUA_INC_ARM)/luajit-2.1

TARGET = noiseintent.so
SRC = noiseintent.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)