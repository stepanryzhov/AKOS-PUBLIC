#ifndef AUCTION_H
#define AUCTION_H

#include <signal.h>
#include <stddef.h>

#define MAX_PARTICIPANTS 32
#define MAX_LOTS 32

typedef enum { WAITING, OPEN, SOLD, UNSOLD, CANCELLED } LotState;
typedef enum { RESERVE_LEADER, RESERVE_UNTIL_CLOSE } ReservationRule;

typedef struct {
    long long start_limit;
    long long next_tick;
    long long last_logged;
} AuctionClock;

typedef struct {
    int lot;  // -1, если участник не готовит ставку.
    int amount;
    int round;
    long long due;
} Bid;

typedef struct {
    int initial_budget;
    int budget;    // Остаток после покупок, включая резерв.
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

// Состояние изменяет только аукционист. Клиенты получают снимки через TCP.
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
    int paused;
    int stopped;
    int stop_signal;
    int failed;
    int connected;  // Только участники и лоты, без наблюдателей.
} Auction;

typedef struct {
    const char* address;
    int port;
    const char* input;
    const char* log_path;
    int delay_ms;
    int wait_seconds;
    ReservationRule reservation_rule;
} Options;

extern volatile sig_atomic_t interrupted;
extern int log_fd;
extern void (*event_output)(const char* text, size_t length);

void config_default(Auction* auction);
int config_load(Auction* auction, const char* path);
int parse_number(const char* text, int minimum, int maximum);
int options_load(int argc, char** argv, Options* options);
int signals_init(void);
void pause_ms(int milliseconds);
int worker_poll_ms(int tick_ms);
long long monotonic_ms(void);
int write_all(int fd, const char* text, size_t length);
int log_open(const char* path, const char* input);
void message(Auction* auction, const char* format, ...);

void auction_intro(Auction* auction);
void close_lot(Auction* auction, int index);
void receive_bid(Auction* auction, int participant);
void advance_auction(Auction* auction, AuctionClock* clock);
void cancel_auction(Auction* auction);
void summary(Auction* auction);

#endif
