#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "queue.h"

static int handle_request(Auction* a, const Request* request) {
    int index = request->index;
    if ((request->is_lot != 0 && request->is_lot != 1) || request->pid <= 0 ||
        index < 0 || index >= (request->is_lot ? a->lot_count : a->participant_count)) return 1;
    pid_t* slot = request->is_lot ? &a->lot_pids[index] : &a->participant_pids[index];
    if (request->kind == JOIN) {
        if (a->started || a->stopped || *slot) return 1;
        *slot = request->pid;
        ++a->connected;
        message(a, "Подключение: %s %d, PID %ld.\n", request->is_lot ? "лот" : "участник",
                index + 1, (long)request->pid);
        return 0;
    }
    if (*slot != request->pid) return 1;
    if (a->stopped) return 0;
    if (request->kind == STOP) {
        if (request->signal_number != SIGINT && request->signal_number != SIGTERM) return 1;
        if (!a->stop_signal) a->stop_signal = request->signal_number;
        return 0;
    }
    if (request->kind == VIEW) return 0;
    if (!a->started) return 1;
    if (request->is_lot) {
        if (request->kind != CLOSE) return 1;
        Lot* lot = &a->lots[index];
        if (lot->state == OPEN && request->bid.round == lot->round && a->now >= lot->closes_at)
            lot->close_requested = 1;
        return 0;
    }
    Participant* p = &a->participants[index];
    if (request->kind == SUBMIT) {
        // Участник сам хранит подготовленную ставку. Устаревшую заявку
        // проверяет receive_bid по тем же правилам, что и в общей памяти.
        if (request->bid.lot < 0 || request->bid.lot >= a->lot_count ||
            request->bid.amount < 0 || request->bid.round < 1 ||
            request->bid.due < 0 || request->bid.due > a->now) return 1;
        p->bid = request->bid;
        p->bid.ready = 1;
        return 0;
    }
    return 1;
}

static Reply make_reply(const Auction* a, const Request* request, int error) {
    Reply reply = {.pid = request->pid, .error = error, .started = a->started,
                   .stopped = a->stopped, .stop_signal = a->stop_signal, .failed = a->failed,
                   .now = a->now, .lot_count = a->lot_count, .tick_ms = a->tick_ms};
    if (error) return reply;
    if (!request->is_lot) {
        const Participant* p = &a->participants[request->index];
        reply.strategy = p->strategy;
        reply.delay = p->delay;
    }
    for (int i = 0; i < a->lot_count; ++i) {
        const Lot* lot = &a->lots[i];
        reply.lots[i] = (LotView){.state = lot->state, .price = lot->price, .step = lot->step,
                                 .leader = lot->leader, .round = lot->round,
                                 .closes_at = lot->closes_at,
                                 .excluded = request->is_lot ? 0 : lot->excluded[request->index]};
    }
    return reply;
}

int queue_auction(Auction* a, const char* session, int wait_seconds) {
    Queues queues;
    if (queues_open(&queues, session, 1) < 0) {
        dprintf(STDERR_FILENO, "Не удалось создать очереди сеанса %s: %s.\n", session, strerror(errno));
        return 1;
    }
    if (ftruncate(log_fd, 0) < 0) {
        perror("Очистка лога");
        queues_close(&queues, 1);
        return 1;
    }
    auction_intro(a);
    message(a, "Аукционист: PID %ld. Очереди: %s, %s.\n", (long)getpid(),
            queues.requests_name, queues.replies_name);
    message(a, "Ожидание процессов: нужно %d, предел %d с.\n",
            a->participant_count + a->lot_count, wait_seconds);
    AuctionClock clock = {.start_limit = monotonic_ms() + wait_seconds * 1000LL, .last_logged = -1};
    for (;;) {
        advance_auction(a, &clock);
        if (a->stopped) {
            if (!a->connected) break;
            if (monotonic_ms() >= clock.stop_limit) {
                message(a, "Не все подключённые процессы подтвердили выход.\n");
                a->failed = 1;
                break;
            }
        }
        long long remaining = a->started && !a->stopped ? clock.next_tick - monotonic_ms() : 50;
        int wait_ms = remaining < 1 ? 1 : remaining > 50 ? 50 : (int)remaining;
        Request request;
        int received = queues_receive(&queues, &request, wait_ms);
        if (received == 1) continue;
        if (received < 0) {
            perror("Получение запроса");
            a->failed = 1;
            if (!a->stopped) cancel_auction(a);
            break;
        }
        advance_auction(a, &clock);
        int error = handle_request(a, &request);
        advance_auction(a, &clock);
        Reply reply = make_reply(a, &request, error);
        if (queues_reply(&queues, &reply) < 0) {
            perror("Отправка ответа");
            a->failed = 1;
            if (!a->stopped) cancel_auction(a);
            break;
        }
        if (!error && a->stopped) {
            pid_t* slot = request.is_lot ? &a->lot_pids[request.index]
                                       : &a->participant_pids[request.index];
            *slot = 0;
            --a->connected;
        }
    }
    summary(a);
    int status = a->failed ? 1 : a->stop_signal ? 128 + a->stop_signal : 0;
    queues_close(&queues, 1);
    return status;
}

static Bid bid_from_reply(const Reply* reply, int index) {
    // Как и choose_bid, выбираем первый доступный лот и один или два шага.
    for (int i = 0; i < reply->lot_count; ++i) {
        const LotView* lot = &reply->lots[i];
        if (lot->state != OPEN || lot->leader == index || lot->excluded) continue;
        return (Bid){.lot = i, .amount = lot->price + (reply->strategy + 1) * lot->step,
                     .round = lot->round, .due = reply->now + reply->delay};
    }
    return (Bid){.lot = -1};
}

int queue_worker(const char* session, int is_lot, int index) {
    Queues queues;
    if (queues_open(&queues, session, 0) < 0) {
        dprintf(STDERR_FILENO, "Не удалось открыть очереди сеанса %s: %s. Сначала запустите auctioneer_mq.\n",
                session, strerror(errno));
        return 1;
    }
    Request request = {.kind = JOIN, .is_lot = is_lot, .index = index, .pid = getpid()};
    Bid pending = {.lot = -1};
    int status = 1;
    for (;;) {
        Reply reply;
        if (queues_request(&queues, &request, &reply) < 0) {
            perror("Обмен сообщениями");
            break;
        }
        if (reply.error) {
            dprintf(STDERR_FILENO, "Номер вне диапазона, занят или запрос не соответствует состоянию.\n");
            break;
        }
        if (reply.stopped) {
            status = reply.failed ? 1 : reply.stop_signal ? 128 + reply.stop_signal : 0;
            break;
        }
        if (request.kind == SUBMIT) pending.lot = -1;
        request.kind = VIEW;
        if (interrupted) {
            request.kind = STOP;
            request.signal_number = interrupted;
        } else if (reply.started) {
            if (is_lot) {
                const LotView* lot = &reply.lots[index];
                if (lot->state == OPEN && reply.now >= lot->closes_at) {
                    request.kind = CLOSE;
                    request.bid.round = lot->round;
                }
            } else {
                if (pending.lot < 0) pending = bid_from_reply(&reply, index);
                if (pending.lot >= 0 && reply.now >= pending.due) {
                    request.kind = SUBMIT;
                    request.bid = pending;
                }
            }
        }
        pause_ms(worker_poll_ms(reply.tick_ms));
    }
    queues_close(&queues, 0);
    return status;
}
