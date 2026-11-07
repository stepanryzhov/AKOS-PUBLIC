#ifndef QUEUE_H
#define QUEUE_H

#include <mqueue.h>

#include "auction.h"

typedef enum { JOIN, VIEW, SUBMIT, CLOSE, STOP } RequestKind;

typedef struct {
    RequestKind kind;
    int is_lot;
    int index;
    pid_t pid;
    int signal_number;
    Bid bid;
} Request;

// Передаётся только нужная участникам часть состояния, без указателей и семафоров.
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
    pid_t pid;
    int error;
    int started;
    int stopped;
    int stop_signal;
    int failed;
    long long now;
    int lot_count;
    int strategy;
    int delay;
    int tick_ms;
    LotView lots[MAX_LOTS];
} Reply;

typedef struct {
    mqd_t requests;
    mqd_t replies;
    sem_t* gate;
    char requests_name[40];
    char replies_name[40];
    char gate_name[40];
} Queues;

int queues_open(Queues* queues, const char* session, int create);
void queues_close(Queues* queues, int owner);
int queues_request(Queues* queues, const Request* request, Reply* reply);
int queues_receive(Queues* queues, Request* request, int milliseconds);
int queues_reply(Queues* queues, const Reply* reply);
int queue_auction(Auction* auction, const char* session, int wait_seconds);
int queue_worker(const char* session, int is_lot, int index);

#endif
