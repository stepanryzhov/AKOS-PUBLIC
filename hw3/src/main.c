#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auction.h"
#ifdef USE_QUEUE
#include "queue.h"
#endif

int main(int argc, char** argv) {
    Mode mode = AUCTION_MODE;
    Options options;
    int parsed = options_load(argc, argv, &options, mode);
    if (parsed) return parsed < 0 ? 1 : 0;
    if (signals_init() < 0) { perror("sigaction"); return 1; }
    Auction initial;
    if (options.input) {
        if (config_load(&initial, options.input)) return 1;
    } else {
        config_default(&initial);
    }
    initial.reservation_rule = options.reservation_rule;
    initial.tick_ms = options.delay_ms ? options.delay_ms : 1;
    // Сначала проверяем лог, но не очищаем его до успешного создания сеанса.
    log_fd = log_open(options.log_path, options.input);
    if (log_fd < 0) { perror("Лог"); return 1; }
    if (!realpath(options.log_path, initial.log_path)) {
        perror("Путь лога");
        close(log_fd);
        return 1;
    }
    char generated[SESSION_LENGTH + 1];
    snprintf(generated, sizeof(generated), "p%ld", (long)getpid());
    const char* session = options.session ? options.session :
                          mode == INDEPENDENT || mode == QUEUED ? "demo" : generated;
#ifdef USE_QUEUE
    int status = queue_auction(&initial, session, options.wait_seconds);
    close(log_fd);
    return status;
#else
    Ipc ipc;
    if (ipc_create(&ipc, session, mode != UNNAMED_FORK, &initial) < 0) {
        dprintf(STDERR_FILENO, "Не удалось создать IPC сеанса %s: %s.\n", session, strerror(errno));
        close(log_fd);
        return 1;
    }
    ipc_lock(&ipc);
    int error = ftruncate(log_fd, 0);
    ipc_unlock(&ipc);
    int status = 1;
    if (error < 0) perror("Очистка лога");
    else status = auction_run(&ipc, mode == INDEPENDENT, options.wait_seconds);
    ipc_close(&ipc, 1);
    close(log_fd);
    return status;
#endif
}
