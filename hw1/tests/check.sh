#!/bin/sh
# Запускается из корня проекта командой make test.
set -eu

test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/auction-tests.XXXXXX")
auction_pid=
cleanup() {
    if [ -n "$auction_pid" ]; then
        kill -TERM "$auction_pid" 2>/dev/null || true
        wait "$auction_pid" 2>/dev/null || true
    fi
    rm -rf "$test_tmp"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# Файлы, на которые ссылается отчёт, воспроизводятся каждым запуском тестов.
./auction -d 0 -l "$test_tmp/builtin.log" > "$test_tmp/builtin.stdout"
./auction -f data/default.txt -d 0 -l tests/default.log > "$test_tmp/default.stdout"
cmp "$test_tmp/builtin.log" tests/default.log
cmp "$test_tmp/builtin.stdout" "$test_tmp/default.stdout"
cmp "$test_tmp/default.stdout" tests/default.log
grep -Fq 'Лот 1: продан, победитель 1, цена 60.' tests/default.log
grep -Fq 'Лот 2: продан, победитель 1, цена 90.' tests/default.log
grep -Fq 'Лот 3: не продан.' tests/default.log
grep -Fq 'Ставки: принято 6, отклонено 10. Продлений 2, отказов 0. Выручка 150.' tests/default.log

# Удержанные деньги недоступны для другого лота, но свою ставку можно повысить
# за счёт её прежнего резерва: новая сумма заменяет старую, а не суммируется с ней.
./auction -f tests/data/reserves.txt -r 1 -d 0 -l tests/hold_reserves.log > "$test_tmp/hold.stdout"
cmp "$test_tmp/hold.stdout" tests/hold_reserves.log
grep -Fq 'Сохранение резерва: участник 1, лот 1, сумма 40 до закрытия раунда.' tests/hold_reserves.log
grep -Fq 'Резервирование: участник 1, лот 1, сумма 60, общий резерв 100, доступно 0.' tests/hold_reserves.log
grep -Fq 'Лот 1: продан, победитель 2, цена 70.' tests/hold_reserves.log
grep -Fq 'Лот 2: продан, победитель 1, цена 40.' tests/hold_reserves.log
grep -Fq 'Участник 1: начальный бюджет 100, потрачено 40, остаток 60, резерв 0.' tests/hold_reserves.log
grep -Fq 'Участник 2: начальный бюджет 90, потрачено 70, остаток 20, резерв 0.' tests/hold_reserves.log
awk '/Закрытие: лот/ { closed = 1 }
     /Освобождение резерва:/ { if (!closed) exit 1; releases++ }
     END { if (releases != 3) exit 1 }' tests/hold_reserves.log

# При отказе освобождаются и деньги проигравших до открытия следующего раунда.
./auction -f tests/data/hold_refusal.txt -r 1 -d 0 -l tests/hold_refusal.log > "$test_tmp/refusal.stdout"
cmp "$test_tmp/refusal.stdout" tests/hold_refusal.log
grep -Fq 'Отказ: участник 1' tests/hold_refusal.log
grep -Fq 'Освобождение резерва: участник 2, лот 1, сумма 30, общий резерв 0.' tests/hold_refusal.log
grep -Fq 'Лот 1: продан, победитель 2, цена 20.' tests/hold_refusal.log
grep -Fq 'Участник 1: начальный бюджет 100, потрачено 0, остаток 100, резерв 0.' tests/hold_refusal.log
grep -Fq 'Участник 2: начальный бюджет 90, потрачено 20, остаток 70, резерв 0.' tests/hold_refusal.log
awk '/Освобождение резерва:/ { released++ }
     /Открытие: лот 1, раунд 2/ { if (released != 2) exit 1; reopened = 1 }
     END { if (!reopened) exit 1 }' tests/hold_refusal.log

check_signal() {
    signal_name=$1
    expected_code=$2
    mode=$3
    log=$4
    if [ "$mode" -eq 0 ]; then
        input=tests/data/extension.txt
        marker='Резервирование:'
    else
        input=tests/data/reserves.txt
        marker='Сохранение резерва:'
    fi
    # Очищаем старый лог до запуска, чтобы не принять прошлое событие за новое.
    : > "$log"
    # Фоновая команда оболочки может наследовать игнорирование SIGINT.
    # Программа явно назначает свой обработчик до первого события в журнале.
    ./auction -f "$input" -r "$mode" -d 200 -l "$log" > "$test_tmp/signal.stdout" &
    auction_pid=$!
    tries=0
    until grep -Fq "$marker" "$log"; do
        if ! kill -0 "$auction_pid" 2>/dev/null || [ "$tries" -ge 50 ]; then
            echo "Не дождались резерва перед сигналом $signal_name." >&2
            exit 1
        fi
        tries=$((tries + 1))
        sleep 0.1
    done
    kill -"$signal_name" "$auction_pid"
    status=0
    wait "$auction_pid" || status=$?
    auction_pid=
    test "$status" -eq "$expected_code"
    cmp "$test_tmp/signal.stdout" "$log"
    grep -Fq '=== ИТОГИ: прервано,' "$log"
    grep -Fq 'Лот 1: отменён.' "$log"
    grep -Fq 'Участник 1: начальный бюджет 100, потрачено 0, остаток 100, резерв 0.' "$log"
    if [ "$mode" -eq 0 ]; then
        grep -Fq 'Участник 2: начальный бюджет 120, потрачено 0, остаток 120, резерв 0.' "$log"
    else
        grep -Fq 'Лот 2: отменён.' "$log"
        grep -Fq 'Участник 2: начальный бюджет 90, потрачено 0, остаток 90, резерв 0.' "$log"
    fi
}

check_signal INT 130 0 tests/interrupt.log
check_signal TERM 143 0 tests/terminate.log
check_signal INT 130 1 tests/hold_interrupt.log
check_signal TERM 143 1 tests/hold_terminate.log

# Некорректный новый параметр должен отклоняться до запуска аукциона.
for invalid_rule in 2 -1 abc; do
    status=0
    ./auction -r "$invalid_rule" -d 0 -l "$test_tmp/invalid.log" > /dev/null 2> "$test_tmp/error" || status=$?
    test "$status" -eq 1
    grep -Fq 'Правило резервирования должно быть 0 или 1.' "$test_tmp/error"
done
