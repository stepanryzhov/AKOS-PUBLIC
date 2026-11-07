#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "auction.h"

volatile sig_atomic_t interrupted = 0;
int log_fd = -1;

static void stop_handler(int signal_number) {
    // Обработчик только запоминает сигнал. Общие данные меняются в цикле.
    interrupted = signal_number;
}

int signals_init(void) {
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = stop_handler;
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) return -1;
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL);
}

void pause_ms(int milliseconds) {
    struct timespec delay = {.tv_sec = milliseconds / 1000,
                             .tv_nsec = (milliseconds % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}

int worker_poll_ms(int tick_ms) {
    // Проверяем состояние несколько раз за такт, не опрашивая его каждую 1 мс.
    int delay = tick_ms / 4;
    if (delay < 1) return 1;
    return delay > 25 ? 25 : delay;
}

long long monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int parse_number(const char* text, int minimum, int maximum) {
    char* end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || end == text || *end || value < minimum || value > maximum) return -1;
    return (int)value;
}

int session_valid(const char* session) {
    size_t length = strlen(session);
    if (length < 1 || length > SESSION_LENGTH) return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)session[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return 1;
}

int log_open(const char* path, const char* input) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return -1;
    struct stat log_stat, input_stat;
    if (fstat(fd, &log_stat) < 0) {
        close(fd);
        return -1;
    }
    // Проверяется и совпадение через символическую или жёсткую ссылку.
    if (input && stat(input, &input_stat) == 0 && log_stat.st_dev == input_stat.st_dev &&
        log_stat.st_ino == input_stat.st_ino) {
        dprintf(STDERR_FILENO, "Входной файл и лог должны различаться.\n");
        close(fd);
        errno = EINVAL;
        return -1;
    }
    return fd;
}

static int write_all(int fd, const char* text, size_t length) {
    while (length) {
        ssize_t written = write(fd, text, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        text += written;
        length -= (size_t)written;
    }
    return 0;
}

void message(Auction* a, const char* format, ...) {
    // Вызывается под общим семафором: сообщения разных процессов не смешиваются.
    char buffer[2048];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length < 0 || length >= (int)sizeof(buffer)) {
        a->failed = 1;
        return;
    }
    int screen_error = write_all(STDOUT_FILENO, buffer, (size_t)length);
    int log_error = write_all(log_fd, buffer, (size_t)length);
    if (screen_error || log_error) {
        if (!a->failed) dprintf(STDERR_FILENO, "Ошибка записи вывода или лога.\n");
        a->failed = 1;
    }
}

int options_load(int argc, char** argv, Options* options, Mode mode) {
    *options = (Options){.log_path = "auction.log", .delay_ms = 200, .wait_seconds = 30};
    int option;
    while ((option = getopt(argc, argv, "f:l:d:r:n:w:h")) != -1) {
        switch (option) {
            case 'f': options->input = optarg; break;
            case 'l': options->log_path = optarg; break;
            case 'd':
                options->delay_ms = parse_number(optarg, 0, 10000);
                if (options->delay_ms < 0) goto invalid;
                break;
            case 'r': {
                int rule = parse_number(optarg, 0, 1);
                if (rule < 0) goto invalid;
                options->reservation_rule = (ReservationRule)rule;
                break;
            }
            case 'n': options->session = optarg; break;
            case 'w':
                options->wait_seconds = parse_number(optarg, 1, 300);
                if (options->wait_seconds < 0) goto invalid;
                break;
            case 'h':
                dprintf(STDOUT_FILENO,
                        "%s [-f файл] [-l лог] [-d мс] [-r 0|1] [-n сеанс] [-w секунды]\n"
                        "Без -f: встроенные данные; лог: auction.log; такт: 200 мс.\n"
                        "-d: 0..10000 (0 означает 1 мс); -r: правило резервов.\n"
                        "-n: 1..16 латинских букв, цифр или _; -w: ожидание процессов 1..300 с.\n"
                        "%s\n",
                        argv[0], mode == QUEUED
                            ? "Затем запустите participant_mq и lot_mq для всех номеров (с 1)."
                            : mode == INDEPENDENT
                            ? "Затем запустите participant и lot для всех номеров (с 1)."
                            : "Все рабочие процессы запускаются автоматически.");
                return 1;
            default: goto invalid;
        }
    }
    if (optind != argc || (options->session && !session_valid(options->session))) goto invalid;
    return 0;
invalid:
    dprintf(STDERR_FILENO, "Некорректные параметры. Справка: %s -h.\n", argv[0]);
    return -1;
}
