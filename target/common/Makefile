TARGET?=

BIN=linkd

SRC:=
SRC+=$(wildcard src/*.c)
SRC+=$(wildcard src/*/*.c)
SRC+=$(wildcard src/*/*/*.c)

CFLAGS?=-Wall -Wextra -O2
LDFLAGS?=

# Drop unreferenced code: dependencies export whole libraries, of which we use
# a part (pbkdf2 also ships SHA-1).
CFLAGS +=-ffunction-sections -fdata-sections
LDFLAGS+=-Wl,--gc-sections

INCLUDES:=
INCLUDES+=-I src

include lib/.dep/config.mk

# Target-specific overrides, optional.
# Supplied by target/<target>/Makefile.target
-include Makefile.target

CFLAGS+=$(INCLUDES)

OBJ:=$(SRC:.c=.o)

.PHONY: default
default: $(BIN)

%.o: %.c $(LIBS)
	$(CC) $(CFLAGS) $(@:.o=.c) -D__NAME=\"$(BIN)\" -D__TARGET=\"$(TARGET)\" -c -o $@

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) $(LDFLAGS) -o $@

.PHONY: clean
clean:
	rm -f $(OBJ) $(BIN)

.PHONY: install
install: $(BIN)
	install -Dm0755 $(BIN) $(DESTDIR)/usr/bin/$(BIN)
