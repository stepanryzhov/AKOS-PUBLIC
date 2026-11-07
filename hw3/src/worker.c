#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "auction.h"
#ifdef USE_QUEUE
#include "queue.h"
#endif

int main(int argc, char** argv) {
    const char* session = "demo";
    int index = -1;
    int option;
    while ((option = getopt(argc, argv, "n:i:h")) != -1) {
        if (option == 'n') session = optarg;
        else if (option == 'i') index = parse_number(optarg, 1, IS_LOT ? MAX_LOTS : MAX_PARTICIPANTS);
        else {
            dprintf(option == 'h' ? STDOUT_FILENO : STDERR_FILENO,
                    "%s [-n сеанс] -i номер\nНумерация с 1. По умолчанию сеанс demo.\n", argv[0]);
            return option == 'h' ? 0 : 1;
        }
    }
    if (optind != argc || !session_valid(session) || index < 1) {
        dprintf(STDERR_FILENO, "Некорректный сеанс или номер процесса.\n");
        return 1;
    }
    if (signals_init() < 0) { perror("sigaction"); return 1; }
#ifdef USE_QUEUE
    return queue_worker(session, IS_LOT, index - 1);
#else
    Ipc ipc;
    if (ipc_connect(&ipc, session) < 0) {
        dprintf(STDERR_FILENO, "Не удалось открыть сеанс %s: %s. Сначала запустите auctioneer.\n",
                session, strerror(errno));
        return 1;
    }
    ipc_lock(&ipc);
    log_fd = log_open(ipc.auction->log_path, NULL);
    ipc_unlock(&ipc);
    if (log_fd < 0) {
        perror("Лог");
        ipc_close(&ipc, 0);
        return 1;
    }
    int status = worker_run(&ipc, IS_LOT, index - 1);
    close(log_fd);
    ipc_close(&ipc, 0);
    return status;
#endif
}
