#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auction.h"

typedef struct {
    Auction* auction;
    int index;
} ThreadArg;

static void pause_ms(int ms) {
    struct timespec pause = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&pause, NULL);
}

static void* participant_thread(void* data) {
    ThreadArg* arg = data;
    Auction* a = arg->auction;
    for (;;) {
        pthread_mutex_lock(&a->mutex);
        if (a->stopped) {
            pthread_mutex_unlock(&a->mutex);
            return NULL;
        }
        prepare_bid(a, arg->index);
        Bid* bid = &a->participants[arg->index].bid;
        if (bid->lot >= 0 && !bid->ready && bid->due <= a->now) {
            // До обработки аукционистом эта заявка больше не меняется.
            bid->ready = 1;
            sem_post(a->events);
        }
        pthread_mutex_unlock(&a->mutex);
        pause_ms(1);
    }
}

static void* lot_thread(void* data) {
    ThreadArg* arg = data;
    Auction* a = arg->auction;
    for (;;) {
        pthread_mutex_lock(&a->mutex);
        if (a->stopped) {
            pthread_mutex_unlock(&a->mutex);
            return NULL;
        }
        Lot* lot = &a->lots[arg->index];
        if (lot->state == OPEN && !lot->close_requested && a->now >= lot->closes_at) {
            lot->close_requested = 1;
            sem_post(a->events);
        }
        pthread_mutex_unlock(&a->mutex);
        pause_ms(1);
    }
}

static void* auctioneer_thread(void* data) {
    Auction* a = data;
    for (;;) {
        // При отсутствии событий аукционист спит, а не опрашивает заявки.
        while (sem_wait(a->events) < 0 && errno == EINTR) {
        }
        pthread_mutex_lock(&a->mutex);
        // Один проход обработает все флаги, накопленные до захвата мьютекса.
        while (sem_trywait(a->events) == 0) {
        }
        if (a->stopped) {
            cancel_auction(a);
            pthread_mutex_unlock(&a->mutex);
            return NULL;
        }
        message(a, "\n=== t=%lld ===\n", a->now);
        for (int i = 0; i < a->lot_count; ++i) {
            Lot* lot = &a->lots[i];
            if (lot->close_requested) {
                lot->close_requested = 0;
                if (lot->state == OPEN && a->now >= lot->closes_at) close_lot(a, i);
            }
        }
        for (int i = 0; i < a->participant_count; ++i) {
            Bid* bid = &a->participants[i].bid;
            if (bid->lot >= 0 && bid->ready) receive_bid(a, i);
        }
        for (int i = 0; i < a->lot_count && a->auctioneer.active < a->simultaneous; ++i) {
            if (a->lots[i].state == WAITING) {
                open_round(a, i);
                ++a->auctioneer.active;
            }
        }
        int finished = a->auctioneer.finished == a->lot_count;
        if (finished) a->stopped = 1;
        pthread_mutex_unlock(&a->mutex);
        if (finished) return NULL;
    }
}

int auction_run(Auction* a, int delay_ms) {
    int error = pthread_mutex_init(&a->mutex, NULL);
    if (error) {
        dprintf(STDERR_FILENO, "Не удалось создать мьютекс: %s.\n", strerror(error));
        return -1;
    }
    // sem_open работает и в Linux, и в macOS. Имя сразу удаляется:
    // после sem_close в системе не останется именованного семафора.
    char name[64];
    snprintf(name, sizeof(name), "/auction-%ld", (long)getpid());
    a->events = sem_open(name, O_CREAT | O_EXCL, 0600, 0);
    if (a->events == SEM_FAILED) {
        dprintf(STDERR_FILENO, "Не удалось создать семафор: %s.\n", strerror(errno));
        pthread_mutex_destroy(&a->mutex);
        return -1;
    }
    sem_unlink(name);
    a->tick_ms = delay_ms ? delay_ms : 1;
    auction_intro(a);

    // Сигналы принимает основной поток. Он меняет флаг остановки под мьютексом
    // и будит аукциониста; в обработчике сигнала нет операций синхронизации.
    sigset_t signals, previous;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, &previous);
    pthread_t workers[MAX_PARTICIPANTS + MAX_LOTS];
    ThreadArg args[MAX_PARTICIPANTS + MAX_LOTS];
    int count = 0;
    for (int i = 0; i < a->participant_count && !error; ++i) {
        args[count] = (ThreadArg){a, i};
        error = pthread_create(&workers[count], NULL, participant_thread, &args[count]);
        if (!error) ++count;
    }
    for (int i = 0; i < a->lot_count && !error; ++i) {
        args[count] = (ThreadArg){a, i};
        error = pthread_create(&workers[count], NULL, lot_thread, &args[count]);
        if (!error) ++count;
    }
    pthread_t auctioneer;
    if (!error) error = pthread_create(&auctioneer, NULL, auctioneer_thread, a);
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (error) {
        dprintf(STDERR_FILENO, "Не удалось создать поток: %s.\n", strerror(error));
        pthread_mutex_lock(&a->mutex);
        a->stopped = 1;
        cancel_auction(a);
        pthread_mutex_unlock(&a->mutex);
    } else {
        sem_post(a->events);  // Первое событие: открыть начальные лоты.
        for (;;) {
            if (!interrupted) pause_ms(a->tick_ms);
            pthread_mutex_lock(&a->mutex);
            if (interrupted) {
                a->stopped = 1;
                sem_post(a->events);
            }
            int stopped = a->stopped;
            // Обычный счётчик тактов. Участники и лоты читают его под мьютексом.
            if (!stopped) ++a->now;
            pthread_mutex_unlock(&a->mutex);
            if (stopped) break;
        }
    }
    if (!error) pthread_join(auctioneer, NULL);
    for (int i = 0; i < count; ++i) pthread_join(workers[i], NULL);
    summary(a);
    sem_close(a->events);
    pthread_mutex_destroy(&a->mutex);
    return error ? -1 : 0;
}
