#ifndef AUCTION_H
#define AUCTION_H

#include <limits.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/types.h>

#define MAX_PARTICIPANTS 32
#define MAX_LOTS 32
#define SESSION_LENGTH 16

typedef enum { WAITING, OPEN, SOLD, UNSOLD, CANCELLED } LotState;
typedef enum { RESERVE_LEADER, RESERVE_UNTIL_CLOSE } ReservationRule;
typedef enum { NAMED_FORK, UNNAMED_FORK, INDEPENDENT, QUEUED } Mode;

typedef struct {
    long long start_limit;
    long long next_tick;
    long long last_logged;
    long long stop_limit;
} AuctionClock;

typedef struct {
    int lot;  // -1, если участник не готовит ставку.
    int amount;
    int round;
    long long due;
    int ready;
} Bid;

typedef struct {
    int initial_budget;
    int budget;    // Деньги после покупок, включая резерв.
    int reserved;  // Сумма резервов по всем открытым лотам.
    int strategy;  // 0 - один шаг, 1 - два шага.
    int delay;
    int refuses;
    Bid bid;
} Participant;

typedef struct {
    int start_price;
    int step;
    int duration;
    int price;
    int leader;
    int winner;
    int round;
    long long closes_at;
    LotState state;
    int close_requested;
    int reserves[MAX_PARTICIPANTS];
    int excluded[MAX_PARTICIPANTS];
} Lot;

typedef struct {
    int active;
    int finished;
    int accepted;
    int rejected;
    int extensions;
    int refusals;
    long long revenue;
} Auctioneer;

// В разделяемой памяти только данные: указатели и файловые дескрипторы
// в независимо запущенных процессах имеют другие значения.
typedef struct {
    Participant participants[MAX_PARTICIPANTS];
    Lot lots[MAX_LOTS];
    Auctioneer auctioneer;
    int participant_count;
    int lot_count;
    int simultaneous;
    int extension;
    int refusal_rule;
    ReservationRule reservation_rule;
    long long now;
    int tick_ms;
    int started;
    int stopped;
    int stop_signal;
    int failed;
    pid_t participant_pids[MAX_PARTICIPANTS];
    pid_t lot_pids[MAX_LOTS];
    int connected;
    char log_path[PATH_MAX];
    sem_t lock_sem;    // Используются только в версии UNNAMED_FORK.
    sem_t events_sem;
} Auction;

// Эти указатели существуют только в адресном пространстве своего процесса.
typedef struct {
    Auction* auction;
    sem_t* lock;
    sem_t* events;
    int named;
    char memory_name[32];
    char lock_name[32];
    char events_name[32];
} Ipc;

typedef struct {
    const char* input;
    const char* log_path;
    const char* session;
    int delay_ms;
    int wait_seconds;
    ReservationRule reservation_rule;
} Options;

extern volatile sig_atomic_t interrupted;
extern int log_fd;

void config_default(Auction* auction);
int config_load(Auction* auction, const char* path);
int parse_number(const char* text, int minimum, int maximum);
int options_load(int argc, char** argv, Options* options, Mode mode);
int signals_init(void);
void pause_ms(int milliseconds);
int worker_poll_ms(int tick_ms);
long long monotonic_ms(void);
int log_open(const char* path, const char* input);
void message(Auction* auction, const char* format, ...);

int session_valid(const char* session);
int ipc_create(Ipc* ipc, const char* session, int named, const Auction* initial);
int ipc_connect(Ipc* ipc, const char* session);
void ipc_close(Ipc* ipc, int owner);
void ipc_lock(Ipc* ipc);
void ipc_unlock(Ipc* ipc);
void ipc_wait_event(Ipc* ipc, int milliseconds);

void auction_intro(Auction* auction);
void open_round(Auction* auction, int index);
void close_lot(Auction* auction, int index);
Bid choose_bid(const Auction* auction, int participant);
void prepare_bid(Auction* auction, int participant);
void receive_bid(Auction* auction, int participant);
void auction_process_events(Auction* auction);
void advance_auction(Auction* auction, AuctionClock* clock);
void cancel_auction(Auction* auction);
void summary(Auction* auction);
int worker_run(Ipc* ipc, int is_lot, int index);
int auction_run(Ipc* ipc, int independent, int wait_seconds);

#endif
