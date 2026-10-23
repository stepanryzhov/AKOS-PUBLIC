#!/bin/sh
# Запуск из корня: make test. Точные победители зависят от планировщика.
set -eu
auction_bin=${AUCTION_BIN:-./auction}
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

# Ограничение ожидания выявляет зависание, а не оставляет тест навсегда.
wait_auction() {
    tries=0
    while kill -0 "$auction_pid" 2>/dev/null; do
        tries=$((tries + 1))
        if [ "$tries" -gt 400 ]; then
            echo 'Аукцион не завершился за 20 секунд.' >&2
            exit 1
        fi
        sleep 0.05
    done
    status=0
    wait "$auction_pid" || status=$?
    auction_pid=
}

check_output() {
    cmp "$test_tmp/stdout" "$log"
    grep -q '^Лот [0-9][0-9]*: ' "$log"
    grep -q '^Участник [0-9][0-9]*: начальный бюджет ' "$log"
    grep -q '^Ставки: принято ' "$log"
    # После завершения или прерывания все резервы должны быть освобождены.
    if grep -Eq '^Участник [0-9]+: начальный бюджет .*резерв [1-9][0-9]*\.$' "$log"; then
        echo "Не освобождены резервы: $log" >&2
        exit 1
    fi
}

run_case() {
    log=$1
    shift
    "$auction_bin" "$@" -l "$log" > "$test_tmp/stdout" &
    auction_pid=$!
    wait_auction
    test "$status" -eq 0
    grep -Fq '=== ИТОГИ: завершено,' "$log"
    check_output
}

run_case tests/builtin.log -d 20
run_case tests/many.log -f tests/data/many.txt -d 20
for mode in 0 1; do
    # Один участник делает одну ставку: цена и итоговый бюджет известны.
    run_case "tests/simple_r${mode}.log" -f tests/data/simple.txt -r "$mode" -d 20
    grep -Fxq 'Лот 1: продан, победитель 1, цена 15.' "$log"
    grep -Fxq 'Участник 1: начальный бюджет 100, потрачено 15, остаток 85, резерв 0.' "$log"
    grep -Fxq 'Ставки: принято 1, отклонено 0. Продлений 0, отказов 0. Выручка 15.' "$log"
    test "$(grep -c '^Оплата:' "$log")" -eq 1

    for name in default reserves unsold deadline extension refusal all_refuse hold_refusal; do
        input=tests/data/$name.txt
        if [ "$name" = default ]; then input=data/default.txt; fi
        run_case "tests/${name}_r${mode}.log" -f "$input" -r "$mode" -d 20
    done
done
# Эти свойства не зависят от того, какой поток первым получил мьютекс.
grep -Fq 'недостаточно доступных средств' tests/unsold_r0.log
grep -Fq 'Лот 1: не продан.' tests/deadline_r0.log
grep -Fq 'Лот 2: не продан.' tests/unsold_r1.log
grep -Fq 'Продление:' tests/extension_r0.log
grep -Fq 'Сохранение резерва:' tests/reserves_r1.log
grep -Fq 'Отказ: участник 1' tests/all_refuse_r0.log
grep -Fq 'Открытие: лот 1, раунд 2' tests/all_refuse_r1.log

# Повторяем быстрые торги: проверяем завершение и вывод при разном порядке потоков.
for mode in 0 1; do
    for run in 1 2 3 4 5; do
        run_case "$test_tmp/fast.log" -f data/default.txt -r "$mode" -d 0
    done
done

for mode in 0 1; do
    for signal_name in INT TERM; do
        log=tests/signal_${signal_name}_r${mode}.log
        : > "$log"
        "$auction_bin" -f tests/data/reserves.txt -r "$mode" -d 200 -l "$log" > "$test_tmp/stdout" &
        auction_pid=$!
        tries=0
        until grep -Fq 'Резервирование:' "$log"; do
            tries=$((tries + 1))
            if [ "$tries" -gt 100 ] || ! kill -0 "$auction_pid" 2>/dev/null; then
                echo 'Не дождались резерва перед сигналом.' >&2
                exit 1
            fi
            sleep 0.01
        done
        kill -"$signal_name" "$auction_pid"
        wait_auction
        if [ "$signal_name" = INT ]; then test "$status" -eq 130; else test "$status" -eq 143; fi
        grep -Fq '=== ИТОГИ: прервано,' "$log"
        grep -Fq 'Лот 1: отменён.' "$log"
        check_output
    done
done

# Прерывание после покупки сохраняет оплату первого лота и отменяет второй.
log=tests/paid_interrupt.log
: > "$log"
"$auction_bin" -f tests/data/paid_interrupt.txt -d 20 -l "$log" > "$test_tmp/stdout" &
auction_pid=$!
tries=0
until grep -Fq 'Оплата:' "$log"; do
    tries=$((tries + 1))
    if [ "$tries" -gt 100 ] || ! kill -0 "$auction_pid" 2>/dev/null; then
        echo 'Не дождались оплаты перед прерыванием.' >&2
        exit 1
    fi
    sleep 0.01
done
kill -TERM "$auction_pid"
wait_auction
test "$status" -eq 143
grep -Fq 'Лот 1: продан, победитель 1, цена 15.' "$log"
grep -Fq 'Лот 2: отменён.' "$log"
check_output

for option in '-r 2' '-r abc' '-d -1' '-d abc'; do
    status=0
    # Здесь намеренно разделяем строку на ключ и значение.
    "$auction_bin" $option > /dev/null 2> "$test_tmp/error" || status=$?
    test "$status" -eq 1
done
printf '1 1 0 0 0\n100 0 1 0\n10 5 3\n' > "$test_tmp/invalid.txt"
status=0
"$auction_bin" -f "$test_tmp/invalid.txt" > /dev/null 2> "$test_tmp/error" || status=$?
test "$status" -eq 1
