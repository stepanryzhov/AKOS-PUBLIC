#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"

#define MAX_CLIENTS 128
#define OUTPUT_SIZE 65536

typedef struct {
    int fd;
    Role role;
    int index;
    int closing;
    int drop;
    long long last_seen;
    char input[256];
    size_t input_used;
    char output[OUTPUT_SIZE];
    size_t output_used;
} Client;

static Client clients[MAX_CLIENTS];

static void queue_bytes(Client* c, const char* text, size_t length) {
    // Медленный наблюдатель отключается, а не задерживает остальных клиентов.
    if (length > sizeof(c->output) - c->output_used) {
        c->drop = 1;
        return;
    }
    memcpy(c->output + c->output_used, text, length);
    c->output_used += length;
}

static void queue_text(Client* c, const char* format, ...) {
    char buffer[LINE_SIZE];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length < 0 || length >= (int)sizeof(buffer)) c->drop = 1;
    else queue_bytes(c, buffer, (size_t)length);
}

static void broadcast_event(const char* text, size_t length) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        Client* c = &clients[i];
        if (c->fd >= 0 && c->role == OBSERVER && !c->closing && !c->drop)
            queue_bytes(c, text, length);
    }
}

static void disconnect_client(Auction* a, Client* c) {
    Role role = c->role;
    int index = c->index;
    close(c->fd);
    *c = (Client){.fd = -1};
    if (role == PARTICIPANT || role == LOT_CLIENT) --a->connected;
    if (a->stopped || role == NO_ROLE) return;
    if (role == OBSERVER) message(a, "Отключение: наблюдатель.\n");
    else message(a, "Отключение: %s %d; состояние сохранено.\n",
                 role == PARTICIPANT ? "участник" : "лот", index + 1);
}

static void reject(Client* c, const char* reason) {
    queue_text(c, "ERROR %s\n", reason);
    c->closing = 1;
}

static void send_state(const Auction* a, Client* c) {
    const Participant* p = c->role == PARTICIPANT ? &a->participants[c->index] : NULL;
    Bid pending = p ? p->bid : (Bid){.lot = -1};
    queue_text(c, "STATE %d %d %lld %d %d %d %d %d %d %d %d %d %lld\n",
               a->started, a->paused, a->now, a->tick_ms, p ? p->strategy : 0,
               p ? p->delay : 0, p ? p->budget : 0, p ? p->reserved : 0, a->lot_count,
               pending.lot, pending.amount, pending.round, pending.due);
    for (int i = 0; i < a->lot_count; ++i) {
        const Lot* lot = &a->lots[i];
        queue_text(c, "LOT %d %d %d %d %d %d %lld\n", (int)lot->state, lot->price, lot->step,
                   lot->leader, lot->round, p ? lot->excluded[c->index] : 0, lot->closes_at);
    }
    queue_text(c, "END\n");
}

static void send_snapshot(const Auction* a, Client* c) {
    queue_text(c, "\n=== СНИМОК: t=%lld, %s ===\n", a->now,
               !a->started ? "ожидание рабочих" : a->paused ? "пауза" : "торги идут");
    queue_text(c, "Участников %d, лотов %d, одновременно %d, продление %d, отказ %d, резервы %d.\n",
               a->participant_count, a->lot_count, a->simultaneous, a->extension,
               a->refusal_rule, (int)a->reservation_rule);
    for (int i = 0; i < a->lot_count; ++i) {
        const Lot* lot = &a->lots[i];
        queue_text(c, "Лот %d: %s, раунд %d, цена %d, лидер %d, победитель %d, закрытие t=%lld; "
                   "начальная цена %d, шаг %d, длительность %d.\n",
                   i + 1, lot_state_name(lot->state), lot->round, lot->price, lot->leader + 1,
                   lot->winner + 1, lot->closes_at, lot->start_price, lot->step, lot->duration);
    }
    for (int i = 0; i < a->participant_count; ++i) {
        const Participant* p = &a->participants[i];
        queue_text(c, "Участник %d: начальный бюджет %d, потрачено %d, остаток %d, резерв %d; "
                   "стратегия %d, задержка %d, отказ %d.\n", i + 1, p->initial_budget,
                   p->initial_budget - p->budget, p->budget, p->reserved,
                   p->strategy, p->delay, p->refuses);
    }
    queue_text(c, "Ставки: принято %d, отклонено %d. Продлений %d, отказов %d. Выручка %lld.\n",
               a->auctioneer.accepted, a->auctioneer.rejected, a->auctioneer.extensions,
               a->auctioneer.refusals, a->auctioneer.revenue);
    queue_text(c, "=== КОНЕЦ СНИМКА; далее события в реальном времени ===\n\n");
}

static int join_client(Auction* a, Client* c, char** fields, int count) {
    if (count != 3 || strcmp(fields[0], "JOIN")) goto invalid;
    int role = parse_number(fields[1], PARTICIPANT, OBSERVER);
    int index = parse_number(fields[2], 0, MAX_PARTICIPANTS);
    if (role < 0 || index < 0) goto invalid;
    if (role == OBSERVER) {
        if (index) goto invalid;
        c->role = OBSERVER;
        message(a, "Подключение: наблюдатель.\n");
        send_snapshot(a, c);
        return 0;
    }
    int limit = role == PARTICIPANT ? a->participant_count : a->lot_count;
    if (index < 1 || index > limit) goto invalid;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].fd >= 0 && clients[i].role == (Role)role && clients[i].index == index - 1) {
            reject(c, "Этот номер уже подключён.");
            return 0;
        }
    }
    c->role = (Role)role;
    c->index = index - 1;
    ++a->connected;
    message(a, "Подключение: %s %d.\n", role == PARTICIPANT ? "участник" : "лот", index);
    return 1;
invalid:
    reject(c, "Некорректная команда, роль или номер.");
    return 0;
}

// Возвращает 1, если после обработки команды рабочему нужен новый снимок.
static int handle_command(Auction* a, Client* c, char* line) {
    char* fields[4];
    int count = 0;
    char* cursor;
    for (char* word = strtok_r(line, " \t\r", &cursor); word;
         word = strtok_r(NULL, " \t\r", &cursor)) {
        if (count == 4) goto invalid;
        fields[count++] = word;
    }
    if (!count) goto invalid;
    if (c->role == NO_ROLE) return join_client(a, c, fields, count);
    if (c->role == OBSERVER) goto invalid;
    if (count == 1 && !strcmp(fields[0], "VIEW")) return 1;
    if (!a->started) goto invalid;
    if (c->role == LOT_CLIENT) {
        if (count != 2 || strcmp(fields[0], "CLOSE")) goto invalid;
        int round = parse_number(fields[1], 1, INT_MAX);
        if (round < 0) goto invalid;
        Lot* lot = &a->lots[c->index];
        if (!a->paused && lot->state == OPEN && lot->round == round && a->now >= lot->closes_at)
            close_lot(a, c->index);
        return 1;
    }
    Participant* p = &a->participants[c->index];
    if (count == 4 && !strcmp(fields[0], "PLAN")) {
        int lot = parse_number(fields[1], 1, a->lot_count);
        int amount = parse_number(fields[2], 0, INT_MAX);
        int round = parse_number(fields[3], 1, INT_MAX);
        if (lot < 0 || amount < 0 || round < 0) goto invalid;
        if (p->bid.lot < 0) {
            // Подготовленная клиентом ставка хранится у сервера и переживает переподключение.
            p->bid = (Bid){.lot = lot - 1, .amount = amount, .round = round, .due = a->now + p->delay};
            message(a, "Подготовка ставки: участник %d, лот %d, сумма %d, поступит t=%lld.\n",
                    c->index + 1, lot, amount, p->bid.due);
        }
        return 1;
    }
    if (count == 1 && !strcmp(fields[0], "BID")) {
        // В паузе ставка остаётся подготовленной; клиент повторит запрос после возобновления.
        if (!a->paused && p->bid.lot >= 0 && a->now >= p->bid.due) receive_bid(a, c->index);
        return 1;
    }
invalid:
    reject(c, "Некорректная команда, роль или номер.");
    return 0;
}

static void read_client(Auction* a, AuctionClock* clock, Client* c) {
    ssize_t size = recv(c->fd, c->input + c->input_used,
                        sizeof(c->input) - 1 - c->input_used, 0);
    if (size < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (size <= 0) { c->drop = 1; return; }
    c->last_seen = monotonic_ms();
    if (a->stopped) return;  // Итоги уже отправлены; ждём закрытия сокета клиентом.
    if (memchr(c->input + c->input_used, '\0', (size_t)size)) {
        reject(c, "Нулевой байт в команде.");
        return;
    }
    c->input_used += (size_t)size;
    char* end;
    while (!c->closing && !c->drop && (end = memchr(c->input, '\n', c->input_used))) {
        *end = '\0';
        advance_auction(a, clock);
        if (a->stopped) return;
        // Время уже обновлено; команда сразу проверяет и меняет состояние аукциона.
        int reply = handle_command(a, c, c->input);
        size_t consumed = (size_t)(end - c->input) + 1;
        c->input_used -= consumed;
        memmove(c->input, end + 1, c->input_used);
        advance_auction(a, clock);
        if (reply && !a->stopped) send_state(a, c);
    }
    if (c->input_used == sizeof(c->input) - 1) reject(c, "Слишком длинная команда.");
}

static void flush_client(Client* c) {
    if (!c->output_used) return;
    ssize_t size = send(c->fd, c->output, c->output_used, 0);
    if (size < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (size <= 0) { c->drop = 1; return; }
    c->output_used -= (size_t)size;
    memmove(c->output, c->output + size, c->output_used);
}

int server_run(Auction* a, int listener, int wait_seconds) {
    for (int i = 0; i < MAX_CLIENTS; ++i) clients[i].fd = -1;
    event_output = broadcast_event;
    auction_intro(a);
    message(a, "Ожидание процессов: нужно %d, предел %d с.\n",
            a->participant_count + a->lot_count, wait_seconds);
    AuctionClock clock = {.start_limit = monotonic_ms() + wait_seconds * 1000LL, .last_logged = -1};
    long long finish_limit = 0;
    for (;;) {
        int active_clients = 0;
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            Client* c = &clients[i];
            if (c->fd < 0) continue;
            if (!a->stopped && c->role != OBSERVER && monotonic_ms() - c->last_seen >= 5000)
                c->drop = 1;
            if (c->drop || (c->closing && !c->output_used)) disconnect_client(a, c);
            else ++active_clients;
        }
        if (!finish_limit) {
            advance_auction(a, &clock);
            if (a->stopped) {
                summary(a);
                int status = a->failed ? 1 : a->stop_signal ? 128 + a->stop_signal : 0;
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    if (clients[i].fd >= 0) queue_text(&clients[i], "DONE %d\n", status);
                }
                close(listener);
                listener = -1;
                finish_limit = monotonic_ms() + 2000;
            }
        }
        if (finish_limit && (!active_clients || monotonic_ms() >= finish_limit)) break;

        struct pollfd descriptors[MAX_CLIENTS + 1];
        descriptors[0] = (struct pollfd){.fd = listener, .events = POLLIN};
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            Client* c = &clients[i];
            descriptors[i + 1] = (struct pollfd){.fd = c->fd,
                .events = (short)((c->closing ? 0 : POLLIN) | (c->output_used ? POLLOUT : 0))};
        }
        long long remaining = a->started && !a->paused && !a->stopped
                                  ? clock.next_tick - monotonic_ms() : 50;
        int timeout = remaining < 1 ? 1 : remaining > 50 ? 50 : (int)remaining;
        if (poll(descriptors, MAX_CLIENTS + 1, timeout) < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            a->failed = 1;
            if (finish_limit) break;
            continue;
        }
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            Client* c = &clients[i];
            if (c->fd < 0) continue;
            short events = descriptors[i + 1].revents;
            if (!c->closing && (events & (POLLIN | POLLHUP))) read_client(a, &clock, c);
            if (!c->drop && (events & POLLOUT)) flush_client(c);
            if (events & (POLLERR | POLLNVAL)) c->drop = 1;
        }
        // Одно новое соединение за итерацию: остальные клиенты тоже получают обслуживание.
        if (listener >= 0 && (descriptors[0].revents & POLLIN)) {
            int fd = net_accept(listener);
            if (fd < 0) {
                if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("accept");
                    a->failed = 1;
                }
            } else {
                int slot = 0;
                while (slot < MAX_CLIENTS && clients[slot].fd >= 0) ++slot;
                if (slot == MAX_CLIENTS) close(fd);
                else clients[slot] = (Client){.fd = fd, .last_seen = monotonic_ms()};
            }
        }
    }
    event_output = NULL;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].fd >= 0) close(clients[i].fd);
    }
    if (listener >= 0) close(listener);
    return a->failed ? 1 : a->stop_signal ? 128 + a->stop_signal : 0;
}
