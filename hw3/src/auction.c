#include <stddef.h>

#include "auction.h"

void open_round(Auction* a, int index) {
    Lot* lot = &a->lots[index];
    lot->state = OPEN;
    lot->price = lot->start_price;
    lot->leader = -1;
    ++lot->round;
    lot->closes_at = a->now + lot->duration;
    message(a, "Открытие: лот %d, раунд %d, цена %d, шаг %d, закрытие t=%lld.\n", index + 1,
            lot->round, lot->price, lot->step, lot->closes_at);
}

static void release_reserve(Auction* a, int index, int participant) {
    Lot* lot = &a->lots[index];
    int amount = lot->reserves[participant];
    if (amount == 0) return;
    Participant* p = &a->participants[participant];
    p->reserved -= amount;
    lot->reserves[participant] = 0;
    message(a, "Освобождение резерва: участник %d, лот %d, сумма %d, общий резерв %d.\n",
            participant + 1, index + 1, amount, p->reserved);
}

static void release_lot_reserves(Auction* a, int index) {
    for (int i = 0; i < a->participant_count; ++i) release_reserve(a, index, i);
}

void close_lot(Auction* a, int index) {
    Lot* lot = &a->lots[index];
    message(a, "Закрытие: лот %d.\n", index + 1);
    if (lot->leader < 0) {
        lot->state = UNSOLD;
        message(a, "Лот %d не продан: принятых ставок нет.\n", index + 1);
    } else {
        int leader = lot->leader;
        Participant* p = &a->participants[leader];
        message(a, "Определение победителя: лот %d, участник %d, цена %d.\n", index + 1, leader + 1,
                lot->price);
        release_lot_reserves(a, index);
        if (a->refusal_rule && p->refuses) {
            ++a->auctioneer.refusals;
            lot->excluded[leader] = 1;
            message(a,
                    "Отказ: участник %d исключён из торгов за лот %d. Повторное "
                    "открытие.\n",
                    leader + 1, index + 1);
            open_round(a, index);
            return;
        }
        p->budget -= lot->price;
        a->auctioneer.revenue += lot->price;
        lot->winner = leader;
        lot->state = SOLD;
        message(a, "Оплата: участник %d, лот %d, сумма %d, остаток бюджета %d.\n", leader + 1,
                index + 1, lot->price, p->budget);
    }
    lot->leader = -1;
    --a->auctioneer.active;
    ++a->auctioneer.finished;
}

void receive_bid(Auction* a, int participant) {
    Participant* p = &a->participants[participant];
    Bid bid = p->bid;
    Lot* lot = &a->lots[bid.lot];
    p->bid.lot = -1;
    message(a, "Поступление ставки: участник %d, лот %d, сумма %d.\n", participant + 1, bid.lot + 1,
            bid.amount);

    // Резерв своей прежней ставки в этом лоте входит в новую сумму,
    // поэтому дополнительно блокируется только разница, а не вся ставка.
    int available = p->budget - p->reserved + lot->reserves[participant];
    const char* reason = NULL;
    if (lot->state != OPEN || a->now >= lot->closes_at)
        reason = "торги закрыты";
    else if (bid.round != lot->round)
        reason = "ставка из предыдущего раунда";
    else if (lot->excluded[participant])
        reason = "участник исключён из торгов за лот";
    else if (bid.amount < lot->price + lot->step)
        reason = "не соблюдён минимальный шаг";
    else if (bid.amount > available)
        reason = "недостаточно доступных средств";
    if (reason) {
        ++a->auctioneer.rejected;
        message(a, "Отклонение: участник %d, лот %d, причина: %s.\n", participant + 1, bid.lot + 1,
                reason);
        return;
    }

    int previous = lot->leader;
    if (a->reservation_rule == RESERVE_LEADER && previous >= 0)
        release_reserve(a, bid.lot, previous);
    else if (previous >= 0 && previous != participant)
        message(a, "Сохранение резерва: участник %d, лот %d, сумма %d до закрытия раунда.\n",
                previous + 1, bid.lot + 1, lot->reserves[previous]);
    lot->price = bid.amount;
    lot->leader = participant;
    p->reserved += bid.amount - lot->reserves[participant];
    lot->reserves[participant] = bid.amount;
    ++a->auctioneer.accepted;
    message(a, "Принятие: участник %d, лот %d, сумма %d.\n", participant + 1, bid.lot + 1,
            bid.amount);
    message(a,
            "Смена лидера: лот %d, участник %d -> участник %d (0 — лидера не "
            "было).\n",
            bid.lot + 1, previous + 1, participant + 1);
    message(a,
            "Резервирование: участник %d, лот %d, сумма %d, общий резерв %d, "
            "доступно %d.\n",
            participant + 1, bid.lot + 1, bid.amount, p->reserved, p->budget - p->reserved);

    // Если ставка пришла в последние extension тактов, до закрытия снова
    // остаётся extension тактов. Продлевают только принятые ставки.
    if (a->now + a->extension > lot->closes_at) {
        lot->closes_at = a->now + a->extension;
        ++a->auctioneer.extensions;
        message(a, "Продление: лот %d, новое закрытие t=%lld.\n", bid.lot + 1, lot->closes_at);
    }
}

Bid choose_bid(const Auction* a, int participant) {
    const Participant* p = &a->participants[participant];
    // Выбираем первый открытый лот, где участник ещё не лидирует.
    for (int index = 0; index < a->lot_count; ++index) {
        const Lot* lot = &a->lots[index];
        if (lot->state != OPEN || lot->leader == participant || lot->excluded[participant])
            continue;
        int amount = lot->price + (p->strategy + 1) * lot->step;
        // Непосильная заявка всё равно поступает аукционисту и отклоняется.
        return (Bid){.lot = index, .amount = amount, .round = lot->round,
                     .due = a->now + p->delay};
    }
    return (Bid){.lot = -1};
}

void prepare_bid(Auction* a, int participant) {
    Participant* p = &a->participants[participant];
    if (p->bid.lot >= 0) return;
    p->bid = choose_bid(a, participant);
    if (p->bid.lot >= 0)
        message(a, "Подготовка ставки: участник %d, лот %d, сумма %d, поступит t=%lld.\n",
                participant + 1, p->bid.lot + 1, p->bid.amount, p->bid.due);
}

void auction_process_events(Auction* a) {
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
}

// Одинаковые правила времени, запуска и остановки для всех способов IPC.
void advance_auction(Auction* a, AuctionClock* clock) {
    long long now = monotonic_ms();
    if (interrupted && !a->stop_signal) a->stop_signal = interrupted;
    if (!a->started && !a->stopped) {
        if (a->connected == a->participant_count + a->lot_count) {
            a->started = 1;
            clock->next_tick = now + a->tick_ms;
            message(a, "Все процессы подключены. Начало торгов.\n");
        } else if (now >= clock->start_limit) {
            message(a, "Не все процессы подключились за отведённое время.\n");
            a->failed = 1;
        }
    }
    if (!a->stopped) {
        if (a->stop_signal || a->failed) {
            cancel_auction(a);
            a->stopped = 1;
        } else if (a->started) {
            // После паузы продвигаем один такт; порядок работы процессов свободный.
            if (now >= clock->next_tick) {
                ++a->now;
                clock->next_tick = now + a->tick_ms;
            }
            if (a->now != clock->last_logged) {
                message(a, "\n=== t=%lld ===\n", a->now);
                clock->last_logged = a->now;
            }
            auction_process_events(a);
            if (a->auctioneer.finished == a->lot_count) a->stopped = 1;
        }
    }
    if (a->stopped && !clock->stop_limit) clock->stop_limit = now + 5000;
}

void cancel_auction(Auction* a) {
    if (a->stop_signal)
        message(a, "Прерывание: сигнал %d. Незавершённые лоты отменяются.\n", a->stop_signal);
    else
        message(a, "Ошибка работы процессов. Незавершённые лоты отменяются.\n");
    for (int i = 0; i < a->lot_count; ++i) {
        Lot* lot = &a->lots[i];
        if (lot->state != OPEN && lot->state != WAITING) continue;
        release_lot_reserves(a, i);
        if (lot->state == OPEN) --a->auctioneer.active;
        lot->leader = -1;
        lot->state = CANCELLED;
        ++a->auctioneer.finished;
        message(a, "Отмена: лот %d.\n", i + 1);
    }
    for (int i = 0; i < a->participant_count; ++i) a->participants[i].bid.lot = -1;
}

void summary(Auction* a) {
    int cancelled = 0;
    for (int i = 0; i < a->lot_count; ++i) {
        if (a->lots[i].state == CANCELLED) cancelled = 1;
    }
    message(a, "\n=== ИТОГИ: %s, t=%lld ===\n", cancelled || a->stop_signal || a->failed ? "прервано" : "завершено",
            a->now);
    for (int i = 0; i < a->lot_count; ++i) {
        Lot* lot = &a->lots[i];
        if (lot->state == SOLD) {
            message(a, "Лот %d: продан, победитель %d, цена %d.\n", i + 1, lot->winner + 1,
                    lot->price);
        } else {
            message(a, "Лот %d: %s.\n", i + 1, lot->state == CANCELLED ? "отменён" : "не продан");
        }
    }
    for (int i = 0; i < a->participant_count; ++i) {
        Participant* p = &a->participants[i];
        message(a,
                "Участник %d: начальный бюджет %d, потрачено %d, остаток %d, "
                "резерв %d.\n",
                i + 1, p->initial_budget, p->initial_budget - p->budget, p->budget, p->reserved);
    }
    message(a,
            "Ставки: принято %d, отклонено %d. Продлений %d, отказов %d. Выручка "
            "%lld.\n",
            a->auctioneer.accepted, a->auctioneer.rejected, a->auctioneer.extensions,
            a->auctioneer.refusals, a->auctioneer.revenue);
}

void auction_intro(Auction* a) {
    message(a,
            "Электронный аукцион: участников %d, лотов %d, одновременно %d.\n"
            "Продление %d, правило отказа %d.\n"
            "Правило резервирования %d: %s.\n"
            "Стратегии: 0 — один шаг, 1 — два шага.\n",
            a->participant_count, a->lot_count, a->simultaneous, a->extension, a->refusal_rule,
            (int)a->reservation_rule,
            a->reservation_rule == RESERVE_LEADER
                ? "полная ставка только у текущего лидера"
                : "резервы текущего и прежних лидеров до закрытия раунда");
    message(a, "Процессы модели: %d участников, %d лотов, 1 аукционист. Такт: %d мс.\n",
            a->participant_count, a->lot_count, a->tick_ms);
    for (int i = 0; i < a->participant_count; ++i) {
        Participant* p = &a->participants[i];
        message(a, "Участник %d: бюджет %d, стратегия %d, задержка %d, отказ %d.\n", i + 1,
                p->budget, p->strategy, p->delay, p->refuses);
    }
}
