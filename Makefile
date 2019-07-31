prefix?=/usr/local

mysw_cflags:=-std=c90 -Wall -Wextra -pedantic -fstack-protector-all -g -O0 -D_GNU_SOURCE -I. $(CFLAGS)
mysw_ldflags:=$(LDFLAGS)
mysw_ldlibs:=-lm -lpthread -lcrypto $(LDLIBS)
mysw_objects:=$(patsubst %.c,%.o,$(wildcard *.c))

all: mysw

mysw: $(mysw_objects)
	$(CC) $(mysw_cflags) $(mysw_objects) $(mysw_ldflags) $(mysw_ldlibs) -o mysw

$(mysw_objects): %.o: %.c
	$(CC) -c $(mysw_cflags) $< -o $@

clean:
	rm -f mysw $(mysw_objects)

.PHONY: all clean
