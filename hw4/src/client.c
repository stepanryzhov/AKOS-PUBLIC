#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "net.h"

// Только данные, полученные от сервера, без чужих бюджетов и таблиц резервов.
typedef struct {
    LotState state;
    int price;
    int step;
    int leader;
    int round;
    int excluded;
    long long closes_at;
} LotView;

typedef struct {
    int started;
    int paused;
    long long now;
    int tick_ms;
    int strategy;
    int delay;
    int budget;
    int reserved;
    int lot_count;
    Bid bid;
    LotView lots[MAX_LOTS];
} View;

static int read_line(int fd, LineReader* reader, char* line, int timeout) {
    int result = net_read_line(fd, reader, line, timeout);
    if (result <= 0 && !interrupted) {
        if (result == 0) dprintf(STDERR_FILENO, "Соединение с сервером закрыто.\n");
        else perror("Получение данных");
    }
    return result;
}

// 1 - снимок, 0 - завершение сервера, -1 - ошибка.
static int read_state(int fd, LineReader* reader, View* view, int* status) {
    char line[LINE_SIZE], extra;
    if (read_line(fd, reader, line, 5000) <= 0) return -1;
    if (sscanf(line, "DONE %d%c", status, &extra) == 1) return 0;
    if (!strncmp(line, "ERROR ", 6)) {
        dprintf(STDERR_FILENO, "%s\n", line + 6);
        return -1;
    }
    if (sscanf(line, "STATE %d %d %lld %d %d %d %d %d %d %d %d %d %lld%c",
               &view->started, &view->paused, &view->now, &view->tick_ms,
               &view->strategy, &view->delay, &view->budget, &view->reserved, &view->lot_count,
               &view->bid.lot, &view->bid.amount, &view->bid.round, &view->bid.due, &extra) != 13 ||
        view->lot_count < 1 || view->lot_count > MAX_LOTS || view->tick_ms < 1 ||
        view->tick_ms > 10000 || view->bid.lot < -1 || view->bid.lot >= view->lot_count)
        goto invalid;
    for (int i = 0; i < view->lot_count; ++i) {
        LotView* lot = &view->lots[i];
        int state;
        if (read_line(fd, reader, line, 5000) <= 0) return -1;
        if (sscanf(line, "LOT %d %d %d %d %d %d %lld%c", &state, &lot->price, &lot->step,
                   &lot->leader, &lot->round, &lot->excluded, &lot->closes_at, &extra) != 7 ||
            state < WAITING || state > CANCELLED) goto invalid;
        lot->state = (LotState)state;
    }
    if (read_line(fd, reader, line, 5000) <= 0) return -1;
    if (strcmp(line, "END")) goto invalid;
    return 1;
invalid:
    dprintf(STDERR_FILENO, "Некорректный ответ сервера.\n");
    return -1;
}

static int observe(int fd) {
    LineReader reader = {0};
    char line[LINE_SIZE], extra;
    while (!interrupted) {
        if (read_line(fd, &reader, line, 0) <= 0) return 1;
        int status;
        if (sscanf(line, "DONE %d%c", &status, &extra) == 1) return status;
        if (!strncmp(line, "ERROR ", 6)) {
            dprintf(STDERR_FILENO, "%s\n", line + 6);
            return 1;
        }
        if (write_all(STDOUT_FILENO, line, strlen(line)) || write_all(STDOUT_FILENO, "\n", 1)) {
            perror("Вывод наблюдателя");
            return 1;
        }
    }
    return 128 + interrupted;
}

static Bid choose_bid(const View* view, int participant) {
    // Правило из HW3: первый открытый лот, где участник не лидирует и не исключён.
    for (int i = 0; i < view->lot_count; ++i) {
        const LotView* lot = &view->lots[i];
        if (lot->state != OPEN || lot->leader == participant || lot->excluded) continue;
        return (Bid){.lot = i, .amount = lot->price + (view->strategy + 1) * lot->step,
                     .round = lot->round};
    }
    return (Bid){.lot = -1};
}

static int work(int fd, Role role, int index) {
    LineReader reader = {0};
    View view = {0};
    int previous_budget = -1, previous_reserved = -1;
    LotView previous_lot = {.state = CANCELLED};
    char command[256];
    // JOIN уже отправлен из main; сначала получаем ответ на него.
    command[0] = '\0';
    while (!interrupted) {
        if (command[0] && net_send(fd, command) < 0) {
            if (!interrupted) perror("Отправка команды");
            return 1;
        }
        int status = 1;
        int result = read_state(fd, &reader, &view, &status);
        if (result <= 0) {
            if (result == 0) dprintf(STDOUT_FILENO, "Сервер завершил аукцион, код %d.\n", status);
            return result == 0 ? status : 1;
        }
        if (role == PARTICIPANT && (view.budget != previous_budget || view.reserved != previous_reserved)) {
            dprintf(STDOUT_FILENO, "Участник %d: t=%lld, бюджет %d, резерв %d, доступно %d.\n",
                    index + 1, view.now, view.budget, view.reserved, view.budget - view.reserved);
            previous_budget = view.budget;
            previous_reserved = view.reserved;
        }
        if (role == LOT_CLIENT) {
            if (index >= view.lot_count) return 1;
            const LotView* lot = &view.lots[index];
            if (lot->state != previous_lot.state || lot->price != previous_lot.price ||
                lot->round != previous_lot.round || lot->closes_at != previous_lot.closes_at) {
                dprintf(STDOUT_FILENO, "Лот %d: t=%lld, %s, раунд %d, цена %d, лидер %d, закрытие t=%lld.\n",
                        index + 1, view.now, lot_state_name(lot->state), lot->round,
                        lot->price, lot->leader + 1, lot->closes_at);
                previous_lot = *lot;
            }
        }
        snprintf(command, sizeof(command), "VIEW\n");
        if (view.started && !view.paused) {
            if (role == LOT_CLIENT) {
                const LotView* lot = &view.lots[index];
                if (lot->state == OPEN && view.now >= lot->closes_at)
                    snprintf(command, sizeof(command), "CLOSE %d\n", lot->round);
            } else if (view.bid.lot >= 0) {
                if (view.now >= view.bid.due) {
                    snprintf(command, sizeof(command), "BID\n");
                    dprintf(STDOUT_FILENO, "Ставка: лот %d, сумма %d, раунд %d.\n",
                            view.bid.lot + 1, view.bid.amount, view.bid.round);
                }
            } else {
                Bid bid = choose_bid(&view, index);
                if (bid.lot >= 0) {
                    snprintf(command, sizeof(command), "PLAN %d %d %d\n",
                             bid.lot + 1, bid.amount, bid.round);
                    dprintf(STDOUT_FILENO, "Подготовка: лот %d, сумма %d, задержка %d тактов.\n",
                            bid.lot + 1, bid.amount, view.delay);
                }
            }
        }
        pause_ms(worker_poll_ms(view.tick_ms));
    }
    return 128 + interrupted;
}

int main(int argc, char** argv) {
    Role role = CLIENT_ROLE;
    const char* address = NULL;
    int port = 0, index = 0, option;
    while ((option = getopt(argc, argv, "a:p:i:h")) != -1) {
        if (option == 'a') address = optarg;
        else if (option == 'p') {
            port = parse_number(optarg, 1, 65535);
            if (port < 0) goto invalid;
        } else if (option == 'i' && role != OBSERVER) {
            index = parse_number(optarg, 1, role == LOT_CLIENT ? MAX_LOTS : MAX_PARTICIPANTS);
            if (index < 0) goto invalid;
        } else if (option == 'h') {
            dprintf(STDOUT_FILENO, "%s -a IPv4 -p порт%s\n",
                    argv[0], role == OBSERVER ? "" : " -i номер (с 1)");
            return 0;
        } else goto invalid;
    }
    if (optind != argc || !address || port < 1 || (role != OBSERVER && index < 1)) goto invalid;
    if (signals_init() < 0) { perror("sigaction"); return 1; }
    int fd = net_connect(address, port);
    if (fd < 0) {
        if (!interrupted) perror("Подключение TCP");
        return interrupted ? 128 + interrupted : 1;
    }
    char join[64];
    snprintf(join, sizeof(join), "JOIN %d %d\n", (int)role, index);
    int status = 1;
    if (net_send(fd, join) < 0) {
        if (!interrupted) perror("Подключение роли");
    } else {
        status = role == OBSERVER ? observe(fd) : work(fd, role, index - 1);
    }
    close(fd);
    return interrupted ? 128 + interrupted : status;
invalid:
    dprintf(STDERR_FILENO, "Некорректные параметры. Справка: %s -h.\n", argv[0]);
    return 1;
}
