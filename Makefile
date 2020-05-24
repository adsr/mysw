mysw_cflags:=-std=c99 -Wall -Wextra -pedantic -Ivendor/libaco -g -O0 -D_GNU_SOURCE $(CFLAGS)
mysw_libs:=-pthread -lm $(LDLIBS)

all: mysw

aco.o: vendor/libaco/aco.c
	$(CC) -c $< -o $@

acosw.o: vendor/libaco/acosw.S
	$(CC) -c $< -o $@

mysw: mysw.c aco.o acosw.o
	$(CC) $(mysw_cflags) mysw.c aco.o acosw.o -o mysw $(mysw_libs)

clean:
	rm -f mysw aco.o acosw.o

.PHONY: all clean
