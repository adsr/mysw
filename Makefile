mysw_cflags:=-std=c99 -Wall -Wextra -pedantic -Ivendor/libaco -Ivendor/uthash/src -g -O0 -D_GNU_SOURCE $(CFLAGS)
mysw_libs:=-pthread -lm $(LDLIBS)
mysw_sources:=mysw.c signal.c worker.c client.c server.c acceptor.c listener.c buf.c targeter.c util.c

all: mysw

aco.o: vendor/libaco/aco.c
	$(CC) -c $< -o $@

acosw.o: vendor/libaco/acosw.S
	$(CC) -c $< -o $@

mysw: $(mysw_sources) aco.o acosw.o
	$(CC) $(mysw_cflags) $(mysw_sources) aco.o acosw.o -o mysw $(mysw_libs)

clean:
	rm -f mysw aco.o acosw.o

.PHONY: all clean
