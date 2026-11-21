#include <stdio.h>
#include <unistd.h>

#include "net.h"

int main(int argc, char** argv) {
    Options options;
    int parsed = options_load(argc, argv, &options);
    if (parsed) return parsed < 0 ? 1 : 0;
    if (signals_init() < 0) { perror("sigaction"); return 1; }
    Auction auction;
    if (options.input) {
        if (config_load(&auction, options.input)) return 1;
    } else {
        config_default(&auction);
    }
    auction.reservation_rule = options.reservation_rule;
    auction.tick_ms = options.delay_ms ? options.delay_ms : 1;
    log_fd = log_open(options.log_path, options.input);
    if (log_fd < 0) { perror("Лог"); return 1; }
    int listener = net_listen(options.address, options.port);
    if (listener < 0) {
        perror("Сервер TCP");
        close(log_fd);
        return 1;
    }
    // Не очищаем существующий журнал, если адрес уже занят другим сервером.
    if (ftruncate(log_fd, 0) < 0) {
        perror("Очистка лога");
        close(listener);
        close(log_fd);
        return 1;
    }
    message(&auction, "Аукционист TCP: %s:%d, PID %ld.\n",
            options.address, options.port, (long)getpid());
    int status = server_run(&auction, listener, options.wait_seconds);
    close(log_fd);
    return status;
}
