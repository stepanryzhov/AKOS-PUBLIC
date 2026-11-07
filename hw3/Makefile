CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700

.PHONY: all test clean
all: auction_named auction_unnamed auctioneer participant lot auctioneer_mq participant_mq lot_mq

auction_named: src/main.c src/config.c src/auction.c src/common.c src/ipc.c src/processes.c src/auction.h
	$(CC) $(CFLAGS) -DAUCTION_MODE=NAMED_FORK src/main.c src/config.c src/auction.c src/common.c src/ipc.c src/processes.c -o auction_named -pthread -lrt

auction_unnamed: src/main.c src/config.c src/auction.c src/common.c src/ipc.c src/processes.c src/auction.h
	$(CC) $(CFLAGS) -DAUCTION_MODE=UNNAMED_FORK src/main.c src/config.c src/auction.c src/common.c src/ipc.c src/processes.c -o auction_unnamed -pthread -lrt

auctioneer: src/main.c src/config.c src/auction.c src/common.c src/ipc.c src/processes.c src/auction.h
	$(CC) $(CFLAGS) -DAUCTION_MODE=INDEPENDENT src/main.c src/config.c src/auction.c src/common.c src/ipc.c src/processes.c -o auctioneer -pthread -lrt

participant: src/worker.c src/config.c src/auction.c src/common.c src/ipc.c src/processes.c src/auction.h
	$(CC) $(CFLAGS) -DIS_LOT=0 src/worker.c src/config.c src/auction.c src/common.c src/ipc.c src/processes.c -o participant -pthread -lrt

lot: src/worker.c src/config.c src/auction.c src/common.c src/ipc.c src/processes.c src/auction.h
	$(CC) $(CFLAGS) -DIS_LOT=1 src/worker.c src/config.c src/auction.c src/common.c src/ipc.c src/processes.c -o lot -pthread -lrt

auctioneer_mq: src/main.c src/config.c src/auction.c src/common.c src/queue_ipc.c src/queue_processes.c src/auction.h src/queue.h
	$(CC) $(CFLAGS) -DUSE_QUEUE -DAUCTION_MODE=QUEUED src/main.c src/config.c src/auction.c src/common.c src/queue_ipc.c src/queue_processes.c -o auctioneer_mq -pthread -lrt

participant_mq: src/worker.c src/config.c src/auction.c src/common.c src/queue_ipc.c src/queue_processes.c src/auction.h src/queue.h
	$(CC) $(CFLAGS) -DUSE_QUEUE -DIS_LOT=0 src/worker.c src/config.c src/auction.c src/common.c src/queue_ipc.c src/queue_processes.c -o participant_mq -pthread -lrt

lot_mq: src/worker.c src/config.c src/auction.c src/common.c src/queue_ipc.c src/queue_processes.c src/auction.h src/queue.h
	$(CC) $(CFLAGS) -DUSE_QUEUE -DIS_LOT=1 src/worker.c src/config.c src/auction.c src/common.c src/queue_ipc.c src/queue_processes.c -o lot_mq -pthread -lrt

test: all
	python3 tests/check.py

clean:
	rm -f auction_named auction_unnamed auctioneer participant lot auctioneer_mq participant_mq lot_mq auction.log
	rm -rf tests/results
