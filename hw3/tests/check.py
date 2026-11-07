#!/usr/bin/env python3
"""Сценарии, инварианты, сигналы и ошибки запуска. Запуск: make test."""
import itertools
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time

from log_checks import check_log

RESULTS = Path("tests/results")
RESULTS.mkdir(exist_ok=True)
PROGRAMS = ("auction_named", "auction_unnamed", "auctioneer", "auctioneer_mq")
WORKERS = {"auctioneer": ("participant", "lot"),
           "auctioneer_mq": ("participant_mq", "lot_mq")}
SESSIONS = itertools.count()
passed = 0


def wait_for(log, text, owner):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if log.exists() and text in log.read_text():
            return
        assert owner.poll() is None, "Процесс завершился до события: " + text
        time.sleep(0.005)
    raise AssertionError("Не дождались события: " + text)


def cleanup(processes):
    # У fork-версий группа может содержать потомков даже после выхода владельца.
    for process in processes:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    for process in processes:
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()


def check_resources(session):
    assert not list(Path("/dev/shm").glob(f"*_{session}_*")), "Остались память или семафоры"
    assert not list(Path("/dev/mqueue").glob(f"a3q_{session}_*")), "Остались очереди"


def run_case(program, case, rule=0, interrupt=None, data=None):
    global passed
    session = f"test{os.getpid()}_{next(SESSIONS)}"
    label = f"{program}_{case}_r{rule}"
    log = RESULTS / (label + ".log")
    if data is None:
        data = (Path("tests/data") / (case + ".txt")).read_text()
    input_path = RESULTS / (label + ".txt")
    input_path.write_text(data)
    log.unlink(missing_ok=True)
    owner = subprocess.Popen(
        ["./" + program, "-f", str(input_path), "-l", str(log), "-n", session,
         "-r", str(rule), "-d", "30", "-w", "5"],
        stdout=subprocess.DEVNULL, start_new_session=True)
    processes = [owner]
    try:
        if program in WORKERS:
            wait_for(log, "Ожидание процессов:", owner)
            counts = map(int, data.split()[:2])
            for worker, count in zip(WORKERS[program], counts):
                for index in range(1, count + 1):
                    processes.append(subprocess.Popen(
                        ["./" + worker, "-n", session, "-i", str(index)],
                        stdout=subprocess.DEVNULL, start_new_session=True))
        if interrupt:
            event, target, signum = interrupt
            wait_for(log, event, owner)
            if target == "owner":
                owner.send_signal(signum)
            else:
                # PID берём из лога: это работает и для fork, и для отдельных программ.
                pid = re.search(rf"Подключение: {target} 1, PID (\d+)\.", log.read_text())
                assert pid, log
                os.kill(int(pid[1]), signum)
        expected = 128 + interrupt[2] if interrupt else 0
        for process in processes:
            assert process.wait(timeout=15) == expected, log
    finally:
        cleanup(processes)

    text = log.read_text()
    assert ("ИТОГИ: прервано," if interrupt else "ИТОГИ: завершено,") in text, log
    check_log(text, data, rule)
    check_resources(session)
    passed += 1
    print(f"OK: {label}", flush=True)
    return text


def check_errors(program):
    global passed
    with tempfile.TemporaryDirectory(dir=RESULTS) as directory:
        root = Path(directory)
        source = root / "input.txt"
        log = root / "output.log"
        session = f"test{os.getpid()}_{next(SESSIONS)}"
        base = ["./" + program, "-n", session, "-l", str(log)]

        def reject(options):
            global passed
            result = subprocess.run(base + options, capture_output=True, text=True, timeout=5)
            assert result.returncode == 1, (options, result.stderr or result.stdout)
            assert result.stderr or "Не все процессы подключились" in result.stdout
            check_resources(session)
            passed += 1

        for data in ("", "0 1 1 0 0", "33 1 1 0 0", "1 1 2 0 0\n100 0 1 0\n10 5 3",
                     "1 1 1 0 0\n100 0 0 0\n10 5 3", "1 1 1 0 0\n100 0 1 0\n10 0 3",
                     "1 1 1 0 0\n100 0 1 0\n10 5 3\nлишнее"):
            source.write_text(data)
            reject(["-f", str(source)])
        for options in (["-d", "-1"], ["-r", "2"], ["-w", "0"], ["-n", "bad/name"],
                        ["-f", str(root / "missing")], ["-l", str(root / "missing/log")]):
            reject(options)

        original = Path("tests/data/simple.txt").read_text()
        source.write_text(original)
        for kind in ("same", "hard", "symbolic"):
            alias = root / kind
            if kind == "same":
                alias = source
            elif kind == "hard":
                os.link(source, alias)
            else:
                alias.symlink_to(source.name)
            reject(["-f", str(source), "-l", str(alias)])
            assert source.read_text() == original
        if program in WORKERS:
            reject(["-w", "1"])
    print(f"OK: {program}, ошибки ввода и запуска", flush=True)


def main():
    for program in PROGRAMS:
        text = run_case(program, "simple")
        assert "Лот 1: продан, победитель 1, цена 15." in text
        assert "потрачено 15, остаток 85, резерв 0." in text
        assert text.count("Оплата:") == 1

        text = run_case(program, "unsold")
        assert "Лот 1: не продан." in text and "Лот 2: не продан." in text
        assert "Оплата:" not in text

        text = run_case(program, "deadline")
        assert "Оплата:" not in text
        for rule in (0, 1):
            text = run_case(program, "extension", rule)
            assert "Продление:" in text and "Оплата:" in text

            text = run_case(program, "reserves", rule)
            changes = re.findall(r"Смена лидера: лот \d+, участник ([1-9]\d*) ->", text)
            assert changes, "Не проверена смена существующего лидера"
            if rule == 1:
                assert "Сохранение резерва:" in text

            text = run_case(program, "refusal", rule)
            assert "Отказ: участник 1" in text and "Лот 1: продан, победитель 2" in text

            text = run_case(program, "all_refuse", rule)
            assert "лот 1, раунд 2" in text and "Лот 1: не продан." in text
            assert "потрачено 0, остаток 100, резерв 0." in text

            text = run_case(program, "all_refuse_multiple", rule)
            assert "Лот 1: не продан." in text and "Лот 2: не продан." in text
            assert "Оплата:" not in text and text.count("Отказ:") >= 2

        run_case(program, "maximum", data="32 32 32 0 0\n" + "0 0 1 0\n"*32 + "10 5 4\n"*32)
        text = run_case(program, "paid_interrupt", rule=1,
                        interrupt=("Оплата:", "owner", signal.SIGTERM))
        assert "Лот 1: продан, победитель 1, цена 15." in text and "Лот 2: отменён." in text
        assert "потрачено 15, остаток 85, резерв 0." in text

        for target in ("owner", "участник", "лот"):
            run_case(program, "sigint_" + target,
                     data="2 2 2 0 0\n100 0 1 0\n90 1 2 0\n10 5 1000\n20 5 1000\n",
                     interrupt=("Принятие:", target, signal.SIGINT))
        check_errors(program)
    print(f"Все {passed} проверок пройдены.")


if __name__ == "__main__":
    main()
