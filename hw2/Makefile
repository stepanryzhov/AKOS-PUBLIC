CC = cc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2
SOURCES = src/main.c src/config.c src/auction.c src/threads.c
LDLIBS = -pthread

.PHONY: all test clean
all: auction

auction: $(SOURCES) src/auction.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@ $(LDLIBS)

test: auction
	sh tests/check.sh
	@echo 'Все тесты пройдены.'

clean:
	rm -f auction auction.log tests/*.log
