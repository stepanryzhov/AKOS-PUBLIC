CC = cc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2
SOURCES = src/main.c src/config.c src/auction.c

.PHONY: all test clean
all: auction

auction: $(SOURCES) src/auction.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@

test: auction
	./auction -f tests/data/reserves.txt -d 0 -l tests/reserves.log > /dev/null
	grep -Fq 'Лот 1: продан, победитель 1, цена 60.' tests/reserves.log
	grep -Fq 'Лот 2: продан, победитель 2, цена 50.' tests/reserves.log
	grep -Fq 'недостаточно доступных средств' tests/reserves.log
	./auction -f tests/data/unsold.txt -d 0 -l tests/unsold.log > /dev/null
	grep -Fq 'Лот 1: не продан.' tests/unsold.log
	grep -Fq 'Лот 2: не продан.' tests/unsold.log
	./auction -f tests/data/deadline.txt -d 0 -l tests/deadline.log > /dev/null
	grep -Fq 'торги закрыты' tests/deadline.log
	grep -Fq 'Ставки: принято 0, отклонено 1.' tests/deadline.log
	./auction -f tests/data/extension.txt -d 0 -l tests/extension.log > /dev/null
	grep -Fq 'Продление:' tests/extension.log
	grep -Fq 'Лот 1: продан, победитель 2, цена 110.' tests/extension.log
	./auction -f tests/data/refusal.txt -d 0 -l tests/refusal.log > /dev/null
	grep -Fq 'Отказ: участник 1' tests/refusal.log
	grep -Fq 'Лот 1: продан, победитель 2, цена 20.' tests/refusal.log
	./auction -f tests/data/all_refuse.txt -d 0 -l tests/all_refuse.log > /dev/null
	grep -Fq 'Отказ: участник 1' tests/all_refuse.log
	grep -Fq 'Лот 1: не продан.' tests/all_refuse.log
	sh tests/check.sh
	@echo 'Все тесты пройдены.'

clean:
	rm -f auction auction.log tests/*.log
