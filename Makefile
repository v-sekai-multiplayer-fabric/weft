ERTS_INCLUDE := $(shell erl -noshell -eval 'io:format("~ts/erts-~ts/include", [code:root_dir(), erlang:system_info(version)])' -s init stop)

CFLAGS ?= -O2 -Wall
CFLAGS += -fPIC -std=c11 -I$(ERTS_INCLUDE)

PRIV := priv
NIF := $(PRIV)/weft_dataplane_nif.so

all: $(NIF)

$(NIF): c_src/weft_dataplane_nif.c
	@mkdir -p $(PRIV)
	$(CC) $(CFLAGS) -shared -pthread -o $@ $<

clean:
	@rm -f $(NIF)

.PHONY: all clean
