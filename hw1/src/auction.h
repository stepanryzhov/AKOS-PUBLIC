#ifndef AUCTION_H
#define AUCTION_H

#include <signal.h>

#define MAX_PARTICIPANTS 32
#define MAX_LOTS 32

typedef enum { WAITING, OPEN, SOLD, UNSOLD, CANCELLED } LotState;
typedef enum { RESERVE_LEADER, RESERVE_UNTIL_CLOSE } ReservationRule;

typedef struct {
    int lot;  // -1, если участник не готовит ставку.
    int amount;
    int round;      // Номер торгов: старые заявки после отказа не действуют.
    long long due;  // Модельное время поступления ставки.
} Bid;

typedef struct {
    int initial_budget;
    int budget;    // Остаток денег после оплаты покупок, включая резерв.
    int reserved;  // Сумма резервов участника по всем открытым лотам.
    int strategy;  // 0 — один минимальный шаг, 1 — два шага.
    int delay;     // Время подготовки ставки в тактах модели.
    int refuses;   // Отказывается от победы при включённом правиле отказа.
    Bid bid;
} Participant;

typedef struct {
    int start_price;
    int step;
    int duration;
    int price;
    int leader;  // Индекс участника или -1.
    int winner;
    int round;
    long long closes_at;
    LotState state;
    int reserves[MAX_PARTICIPANTS];  // Резерв последней принятой ставки каждого участника.
    int excluded[MAX_PARTICIPANTS];  // Отказавшийся больше не торгуется за этот лот.
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

typedef struct {
    Participant participants[MAX_PARTICIPANTS];
    Lot lots[MAX_LOTS];
    Auctioneer auctioneer;
    int participant_count;
    int lot_count;
    int simultaneous;
    int extension;
    int refusal_rule;  // 0 — все оплачивают, 1 — отказ и повторное открытие.
    ReservationRule reservation_rule;
    long long now;
    int log_fd;
} Auction;

extern volatile sig_atomic_t interrupted;

void config_default(Auction* auction);
int config_load(Auction* auction, const char* path);
void message(Auction* auction, const char* format, ...);
void auction_run(Auction* auction, int delay_ms);

#endif
