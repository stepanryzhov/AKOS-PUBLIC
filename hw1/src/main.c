#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auction.h"

volatile sig_atomic_t interrupted = 0;

void stop_handler(int signal_number) {
    // В обработчике нет ввода-вывода: завершение выполняет основной цикл.
    interrupted = signal_number;
}

void message(Auction* a, const char* format, ...) {
    // Одно сообщение выводится и на экран, и в файл лога.
    va_list args;
    va_start(args, format);
    vdprintf(STDOUT_FILENO, format, args);
    va_end(args);
    va_start(args, format);
    vdprintf(a->log_fd, format, args);
    va_end(args);
}

void usage(int fd, const char* name) {
    dprintf(fd,
            "Использование: %s [-f файл] [-l лог] [-d миллисекунды] [-r 0|1] [-h]\n"
            "Без -f используются встроенные данные. Лог: auction.log.\n"
            "Задержка между тактами: 200 мс, -d 0 — без ожидания.\n"
            "Резервы: -r 0 — только у лидера (по умолчанию),\n"
            "-r 1 — сохраняются и у прежних лидеров до закрытия раунда.\n",
            name);
}

int main(int argc, char** argv) {
    const char* input = NULL;
    const char* log_path = "auction.log";
    int delay_ms = 200;
    ReservationRule reservation_rule = RESERVE_LEADER;
    int option;
    while ((option = getopt(argc, argv, "f:l:d:r:h")) != -1) {
        switch (option) {
            case 'f':
                input = optarg;
                break;
            case 'l':
                log_path = optarg;
                break;
            case 'd':
                delay_ms = atoi(optarg);
                break;
            case 'r':
                if (strcmp(optarg, "0") != 0 && strcmp(optarg, "1") != 0) {
                    dprintf(STDERR_FILENO, "Правило резервирования должно быть 0 или 1.\n");
                    return 1;
                }
                reservation_rule = strcmp(optarg, "0") == 0 ? RESERVE_LEADER : RESERVE_UNTIL_CLOSE;
                break;
            case 'h':
                usage(STDOUT_FILENO, argv[0]);
                return 0;
            default:
                usage(STDERR_FILENO, argv[0]);
                return 1;
        }
    }
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = stop_handler;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    Auction auction;
    if (input) {
        if (config_load(&auction, input)) return 1;
    } else {
        config_default(&auction);
    }
    auction.reservation_rule = reservation_rule;
    auction.log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (auction.log_fd < 0) {
        dprintf(STDERR_FILENO, "Не удалось открыть лог %s.\n", log_path);
        return 1;
    }
    auction_run(&auction, delay_ms);
    close(auction.log_fd);
    return interrupted ? 128 + interrupted : 0;
}
