CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2 -D_POSIX_C_SOURCE=200809L
PYTHON ?= python3

IP ?= 127.0.0.1
PORT ?= 5000
DATA ?= data/default.txt
DELAY ?= 200
RULE ?= 0
WAIT ?= 30
LOG ?= auction.log
ID ?= 1

COMMON = src/common.c src/net.c
MODEL = src/config.c src/auction.c
HEADERS = src/auction.h src/net.h
PROGRAMS = auctioneer participant lot observer

.PHONY: all run demo server run-participant run-lot run-observer test clean help
all: $(PROGRAMS)

auctioneer: src/main.c src/server.c $(MODEL) $(COMMON) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/main.c src/server.c $(MODEL) $(COMMON) $(LDFLAGS) $(LDLIBS) -o $@

participant: ROLE = PARTICIPANT
lot: ROLE = LOT_CLIENT
observer: ROLE = OBSERVER
participant lot observer: src/client.c $(COMMON) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DCLIENT_ROLE=$(ROLE) src/client.c $(COMMON) $(LDFLAGS) $(LDLIBS) -o $@

run: all
	$(PYTHON) scripts/run.py "$(IP)" "$(PORT)" "$(DATA)" "$(DELAY)" "$(RULE)" "$(LOG)" "$(WAIT)"

demo: run

server: auctioneer
	./auctioneer -a "$(IP)" -p "$(PORT)" -f "$(DATA)" -d "$(DELAY)" -r "$(RULE)" -l "$(LOG)" -w "$(WAIT)"

run-participant: participant
	./participant -a "$(IP)" -p "$(PORT)" -i "$(ID)"

run-lot: lot
	./lot -a "$(IP)" -p "$(PORT)" -i "$(ID)"

run-observer: observer
	./observer -a "$(IP)" -p "$(PORT)"

test: all
	$(PYTHON) tests/check.py

clean:
	rm -f $(PROGRAMS) auction.log
	rm -rf tests/results tests/__pycache__

help:
	@echo 'make                 сборка четырёх программ'
	@echo 'make run             сервер, все рабочие и один наблюдатель'
	@echo 'make server          только сервер'
	@echo 'make run-participant ID=1 / run-lot ID=1 / run-observer'
	@echo 'make test / clean'
	@echo 'Параметры: IP=127.0.0.1 PORT=5000 DATA=data/default.txt DELAY=200 RULE=0 WAIT=30 LOG=auction.log'
