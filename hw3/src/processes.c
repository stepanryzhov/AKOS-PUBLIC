#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "auction.h"

int worker_run(Ipc* ipc, int is_lot, int index) {
    Auction* a = ipc->auction;
    ipc_lock(ipc);
    int count = is_lot ? a->lot_count : a->participant_count;
    pid_t* pids = is_lot ? a->lot_pids : a->participant_pids;
    if (index < 0 || index >= count || a->started || a->stopped || pids[index]) {
        ipc_unlock(ipc);
        dprintf(STDERR_FILENO, "Номер вне диапазона, занят или торги уже начались.\n");
        return 1;
    }
    pids[index] = getpid();
    ++a->connected;
    message(a, "Подключение: %s %d, PID %ld.\n", is_lot ? "лот" : "участник", index + 1,
            (long)getpid());
    sem_post(ipc->events);
    ipc_unlock(ipc);

    for (;;) {
        ipc_lock(ipc);
        if (interrupted && !a->stop_signal) {
            a->stop_signal = interrupted;
            sem_post(ipc->events);
        }
        if (a->stopped) {
            pids[index] = 0;
            --a->connected;
            int status = a->failed ? 1 : a->stop_signal ? 128 + a->stop_signal : 0;
            ipc_unlock(ipc);
            return status;
        }
        if (a->started && !a->stop_signal && !a->failed) {
            if (is_lot) {
                Lot* lot = &a->lots[index];
                if (lot->state == OPEN && !lot->close_requested && a->now >= lot->closes_at) {
                    lot->close_requested = 1;
                    sem_post(ipc->events);
                }
            } else {
                prepare_bid(a, index);
                Bid* bid = &a->participants[index].bid;
                if (bid->lot >= 0 && !bid->ready && bid->due <= a->now) {
                    // До обработки аукционистом участник эту заявку не меняет.
                    bid->ready = 1;
                    sem_post(ipc->events);
                }
            }
        }
        ipc_unlock(ipc);
        pause_ms(worker_poll_ms(a->tick_ms));
    }
}

static int start_children(Ipc* ipc, pid_t* children) {
    Auction* a = ipc->auction;
    int count = a->participant_count + a->lot_count;
    for (int i = 0; i < count; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            dprintf(STDERR_FILENO, "Не удалось создать процесс: %s.\n", strerror(errno));
            return i;
        }
        if (pid == 0) {
            int is_lot = i >= a->participant_count;
            int index = is_lot ? i - a->participant_count : i;
            int status = worker_run(ipc, is_lot, index);
            ipc_close(ipc, 0);
            close(log_fd);
            _exit(status);
        }
        children[i] = pid;
    }
    return count;
}

int auction_run(Ipc* ipc, int independent, int wait_seconds) {
    Auction* a = ipc->auction;
    int expected = a->participant_count + a->lot_count;
    pid_t children[MAX_PARTICIPANTS + MAX_LOTS] = {0};
    int child_count = independent ? 0 : start_children(ipc, children);
    ipc_lock(ipc);
    if (!independent && child_count != expected) a->failed = 1;
    auction_intro(a);
    message(a, "Аукционист: PID %ld. Память: %s.\n", (long)getpid(), ipc->memory_name);
    message(a, "Ожидание процессов: нужно %d, предел %d с.\n", expected, wait_seconds);
    ipc_unlock(ipc);

    AuctionClock clock = {.start_limit = monotonic_ms() + wait_seconds * 1000LL, .last_logged = -1};
    for (;;) {
        int child_failed = 0;
        for (int i = 0; i < child_count; ++i) {
            if (!children[i]) continue;
            int status;
            pid_t pid = waitpid(children[i], &status, WNOHANG);
            if (pid > 0) {
                children[i] = 0;
                child_failed = 1;  // До остановки дочерние процессы не должны выходить.
            }
        }
        ipc_lock(ipc);
        if (child_failed) a->failed = 1;
        while (sem_trywait(ipc->events) == 0) {}
        advance_auction(a, &clock);
        int stopped = a->stopped;
        long long remaining = a->started ? clock.next_tick - monotonic_ms() : 50;
        ipc_unlock(ipc);
        if (stopped) break;
        // Даже без новых заявок проверяем сигналы не реже чем раз в 50 мс.
        int wait_ms = remaining < 1 ? 1 : remaining > 50 ? 50 : (int)remaining;
        ipc_wait_event(ipc, wait_ms);
    }

    for (int i = 0; i < child_count; ++i) {
        if (children[i]) {
            while (waitpid(children[i], NULL, 0) < 0 && errno == EINTR) {}
        }
    }
    if (independent) {
        // Чужие процессы нельзя ожидать через waitpid: они отмечают выход в памяти.
        long long limit = monotonic_ms() + 5000;
        for (;;) {
            ipc_lock(ipc);
            int connected = a->connected;
            ipc_unlock(ipc);
            if (!connected) break;
            if (monotonic_ms() >= limit) {
                ipc_lock(ipc);
                message(a, "Не все подключённые процессы подтвердили выход.\n");
                a->failed = 1;
                ipc_unlock(ipc);
                break;
            }
            pause_ms(1);
        }
    }
    ipc_lock(ipc);
    summary(a);
    int status = a->failed ? 1 : a->stop_signal ? 128 + a->stop_signal : 0;
    ipc_unlock(ipc);
    return status;
}
