#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auction.h"

// strtol пропускает пробелы, читает число и передвигает cursor за него.
int read_number(char** cursor) {
    if (!*cursor) return -1;
    char* end;
    errno = 0;
    long value = strtol(*cursor, &end, 10);
    if (errno || end == *cursor || value < 0 || value > INT_MAX) {
        *cursor = NULL;
        return -1;
    }
    *cursor = end;
    return (int)value;
}

void init_state(Auction* a) {
    for (int i = 0; i < a->participant_count; ++i) {
        a->participants[i].budget = a->participants[i].initial_budget;
        a->participants[i].bid.lot = -1;
    }
    for (int i = 0; i < a->lot_count; ++i) {
        a->lots[i].leader = -1;
        a->lots[i].winner = -1;
    }
}

void config_default(Auction* a) {
    memset(a, 0, sizeof(*a));
    a->participant_count = 3;
    a->lot_count = 3;
    a->simultaneous = 2;
    a->extension = 2;
    a->refusal_rule = 1;
    a->participants[0] = (Participant){.initial_budget = 160, .strategy = 0, .delay = 1};
    a->participants[1] = (Participant){.initial_budget = 220, .strategy = 1, .delay = 2};
    a->participants[2] = (Participant){.initial_budget = 120, .strategy = 0, .delay = 3};
    a->lots[0] = (Lot){.start_price = 30, .step = 10, .duration = 6};
    a->lots[1] = (Lot){.start_price = 50, .step = 10, .duration = 5};
    a->lots[2] = (Lot){.start_price = 20, .step = 5, .duration = 4};
    init_state(a);
}

int config_load(Auction* a, const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "Не удалось открыть %s.\n", path);
        return -1;
    }
    char buffer[16384];
    ssize_t size = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (size < 0 || size == (ssize_t)sizeof(buffer) - 1) {
        dprintf(STDERR_FILENO, "Не удалось прочитать %s.\n", path);
        return -1;
    }
    buffer[size] = '\0';
    char* cursor = buffer;
    memset(a, 0, sizeof(*a));
    a->participant_count = read_number(&cursor);
    a->lot_count = read_number(&cursor);
    a->simultaneous = read_number(&cursor);
    a->extension = read_number(&cursor);
    a->refusal_rule = read_number(&cursor);

    if (a->participant_count < 1 || a->participant_count > MAX_PARTICIPANTS || a->lot_count < 1 ||
        a->lot_count > MAX_LOTS) {
        dprintf(STDERR_FILENO, "Число участников и лотов должно быть от 1 до 32.\n");
        return -1;
    }
    int valid = a->simultaneous >= 1 && a->simultaneous <= a->lot_count && a->extension >= 0 &&
                a->extension <= 1000 && (a->refusal_rule == 0 || a->refusal_rule == 1);

    for (int i = 0; i < a->participant_count; ++i) {
        Participant* p = &a->participants[i];
        p->initial_budget = read_number(&cursor);
        p->strategy = read_number(&cursor);
        p->delay = read_number(&cursor);
        p->refuses = read_number(&cursor);
        if (p->initial_budget < 0 || p->initial_budget > 1000000 || p->strategy < 0 ||
            p->strategy > 1 || p->delay < 1 || p->delay > 1000 || p->refuses < 0 || p->refuses > 1)
            valid = 0;
    }
    for (int i = 0; i < a->lot_count; ++i) {
        Lot* lot = &a->lots[i];
        lot->start_price = read_number(&cursor);
        lot->step = read_number(&cursor);
        lot->duration = read_number(&cursor);
        if (lot->start_price < 0 || lot->start_price > 1000000 || lot->step < 1 ||
            lot->step > 1000000 || lot->duration < 1 || lot->duration > 1000)
            valid = 0;
    }
    if (cursor) {
        while (isspace((unsigned char)*cursor)) ++cursor;
    }
    if (!valid || !cursor || *cursor) {
        dprintf(STDERR_FILENO, "Некорректные параметры входного файла. См. README.md.\n");
        return -1;
    }
    init_state(a);
    return 0;
}
