CC ?= cc
CFLAGS ?= -Iinclude -Isrc -O2 -fPIC -std=gnu11 -Wall -Wextra -fvisibility=hidden
LDFLAGS ?= -shared
LIBS ?= -ldl -pthread

OBJ=src/recorder.o src/hook.o src/pattern.o

all: ql-server-demo-recorder.so

ql-server-demo-recorder.so: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) -Wl,--no-as-needed $(LIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) ql-server-demo-recorder.so tools/*_test tools/maps_self_test

.PHONY: all clean
