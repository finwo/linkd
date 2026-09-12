BIN=bcm.so

# Path to a built OpenBCM SDK tree. Supplied by the openbcm package build;
# there is no sensible default, so the build fails loudly without it.
SDK?=

CFLAGS?=-Wall -Wextra -O2
CFLAGS+=-fPIC

# unosd's dataplane contract
INCLUDES:=
INCLUDES+=-I ../../src

LDFLAGS?=
LDFLAGS+=-shared

ifneq ($(SDK),)
INCLUDES+=-I $(SDK)/include
LDFLAGS +=-L $(SDK)/build -lbcm
endif

CFLAGS+=$(INCLUDES)

SRC:=$(wildcard src/*.c)
OBJ:=$(SRC:.c=.o)

.PHONY: default
default: check $(BIN)

.PHONY: check
check:
	@test -n "$(SDK)" || { \
	  echo "SDK= is required: point it at a built OpenBCM SDK tree"; \
	  exit 1; \
	}

%.o: %.c
	$(CC) $(CFLAGS) $< -c -o $@

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) $(LDFLAGS) -o $@

.PHONY: clean
clean:
	rm -f $(OBJ) $(BIN)

.PHONY: install
install: $(BIN)
	install -Dm0755 $(BIN) $(DESTDIR)/usr/lib/unos/dataplane/$(BIN)
