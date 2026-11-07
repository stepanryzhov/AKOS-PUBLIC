"""Проверка правил аукциона по событиям, без привязки к порядку процессов."""
import re


def check_log(text, data, rule):
    values = list(map(int, data.split()))
    count, lot_count, simultaneous, extension, refusal_rule = values[:5]
    participants = [values[5 + 4*i:9 + 4*i] for i in range(count)]
    start = 5 + 4*count
    lots = [values[start + 3*i:start + 3*i + 3] for i in range(lot_count)]
    budgets = [p[0] for p in participants]
    reserves = [[0]*count for _ in lots]
    states = ["waiting"]*lot_count
    prices, leaders, deadlines, rounds = [0]*lot_count, [-1]*lot_count, [0]*lot_count, [0]*lot_count
    excluded = [set() for _ in lots]
    payments = {}
    totals = [0, 0, 0, 0, 0]  # принято, отклонено, продлено, отказов, выручка
    final_participants, final_lots = set(), set()
    now = 0
    summary = False

    for line in text.splitlines():
        numbers = list(map(int, re.findall(r"-?\d+", line)))
        if line.startswith("=== t="):
            now, = numbers
        elif line.startswith("=== ИТОГИ:"):
            summary = True
        elif line.startswith("Открытие:"):
            k, round_, price, step, end = numbers
            k -= 1
            assert k not in payments and round_ == rounds[k] + 1
            assert states[k] in ("waiting", "closed")
            assert price == lots[k][0] and step == lots[k][1] and end == now + lots[k][2]
            states[k], prices[k], leaders[k], deadlines[k], rounds[k] = "open", price, -1, end, round_
            assert states.count("open") <= simultaneous
        elif line.startswith("Закрытие:"):
            k = numbers[0] - 1
            assert states[k] == "open" and now >= deadlines[k]
            states[k] = "closed"
        elif line.startswith("Принятие:"):
            p, k, amount = numbers
            p -= 1
            k -= 1
            assert states[k] == "open" and now < deadlines[k] and p not in excluded[k]
            assert amount >= prices[k] + lots[k][1]
            assert amount <= budgets[p] - sum(row[p] for row in reserves) + reserves[k][p]
            prices[k] = amount
            totals[0] += 1
        elif line.startswith("Смена лидера:"):
            k, previous, leader = numbers[:3]
            assert leaders[k-1] == previous - 1
            leaders[k-1] = leader - 1
        elif line.startswith(("Освобождение резерва:", "Резервирование:")):
            p, k, amount, total = numbers[:4]
            p -= 1
            k -= 1
            if line.startswith("Освобождение"):
                assert reserves[k][p] == amount
                reserves[k][p] = 0
            else:
                assert states[k] == "open" and leaders[k] == p and prices[k] == amount
                reserves[k][p] = amount
                assert numbers[4] == budgets[p] - total
                if rule == 0:
                    assert sum(value > 0 for value in reserves[k]) <= 1
            assert sum(row[p] for row in reserves) == total <= budgets[p]
            assert total >= 0
        elif line.startswith("Сохранение резерва:"):
            p, k, amount = numbers
            assert rule == 1 and reserves[k-1][p-1] == amount
        elif line.startswith("Продление:"):
            k, end = numbers
            k -= 1
            assert states[k] == "open" and end > deadlines[k] and end == now + extension
            deadlines[k] = end
            totals[2] += 1
        elif line.startswith("Определение победителя:"):
            k, p, price = numbers
            assert states[k-1] == "closed" and leaders[k-1] == p-1 and prices[k-1] == price
        elif line.startswith("Отказ:"):
            p, k = numbers
            assert refusal_rule and participants[p-1][3] and leaders[k-1] == p-1
            assert p-1 not in excluded[k-1]
            excluded[k-1].add(p-1)
            totals[3] += 1
        elif line.startswith("Оплата:"):
            p, k, amount, remaining = numbers
            p -= 1
            k -= 1
            assert k not in payments and states[k] == "closed" and leaders[k] == p
            assert amount == prices[k] and budgets[p] - amount == remaining >= 0
            assert sum(row[p] for row in reserves) <= remaining
            budgets[p] = remaining
            payments[k] = (p+1, amount)
            totals[4] += amount
        elif line.startswith("Отклонение:"):
            totals[1] += 1
        elif line.startswith("Отмена:"):
            k = numbers[0] - 1
            assert states[k] in ("waiting", "open")
            states[k] = "cancelled"
        elif summary and line.startswith("Участник "):
            p, initial, spent, remaining, reserved = numbers
            assert p not in final_participants
            assert initial == participants[p-1][0] and remaining == budgets[p-1]
            assert initial-spent == remaining and reserved == 0
            final_participants.add(p)
        elif summary and line.startswith("Лот "):
            k = numbers[0] - 1
            assert k not in final_lots
            if k in payments:
                assert "продан, победитель" in line and tuple(numbers[1:]) == payments[k]
            else:
                assert ("отменён" if states[k] == "cancelled" else "не продан") in line
            final_lots.add(k)
        elif line.startswith("Ставки:"):
            assert numbers == totals

    assert summary and len(final_participants) == count and len(final_lots) == lot_count
    assert "open" not in states and "waiting" not in states
    assert not any(any(row) for row in reserves)
    assert totals[4] == sum(p[0]-budget for p, budget in zip(participants, budgets))
