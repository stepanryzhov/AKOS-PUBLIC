#ifndef NET_H
#define NET_H

#include "auction.h"

#define LINE_SIZE 4096

typedef enum { NO_ROLE, PARTICIPANT, LOT_CLIENT, OBSERVER } Role;

typedef struct {
    char buffer[LINE_SIZE];
    size_t used;
} LineReader;

// Протокол текстовый: одна строка - команда, снимок заканчивается строкой END.
// TCP может разделять и объединять строки, поэтому чтение хранит остаток.
// Команды: JOIN роль номер, VIEW, PLAN лот сумма раунд, BID, CLOSE раунд.
// Рабочие получают STATE + строки LOT + END, наблюдатели - события и итоговый DONE код.
int net_listen(const char* address, int port);
int net_accept(int listener);
int net_connect(const char* address, int port);
int net_send(int fd, const char* text);
int net_read_line(int fd, LineReader* reader, char* line, int timeout_ms);
const char* lot_state_name(LotState state);
int server_run(Auction* auction, int listener, int wait_seconds);

#endif
