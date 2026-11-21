#!/usr/bin/env python3
"""Небольшой набор сценариев аукциона и проверок TCP. Запуск: make test."""
import re
from pathlib import Path
import signal
import socket
import subprocess
import time

from log_checks import check_log

RESULTS = Path("tests/results")
RESULTS.mkdir(exist_ok=True)
passed = 0


def wait_for(path, text, process, count=1):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        contents = path.read_text() if path.exists() else ""
        if contents.count(text) >= count:
            return contents
        assert process.poll() is None, (path, text, contents)
        time.sleep(0.005)
    raise AssertionError(f"Не дождались {text!r} в {path}")


class Session:
    def __init__(self, name, rule=0, data=None, delay=30, wait=5):
        self.name, self.rule = name, rule
        self.data = data if data is not None else Path(f"tests/data/{name}.txt").read_text()
        self.path = RESULTS / f"{name}_r{rule}"
        self.log = self.path.with_suffix(".log")
        self.log.unlink(missing_ok=True)
        self.source = self.path.with_suffix(".txt")
        self.source.write_text(self.data)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.port = probe.getsockname()[1]
        self.endpoint = ["-a", "127.0.0.1", "-p", str(self.port)]
        self.processes, self.files, self.detached = [], [], {}
        self.delay, self.wait = delay, wait

    def launch(self, program, index=None):
        output = self.path.with_name(f"{self.path.name}_{program}_{len(self.processes)}.log")
        stream = output.open("w")
        self.files.append(stream)
        args = ["./" + program, *self.endpoint]
        if index is not None:
            args += ["-i", str(index)]
        if program == "auctioneer":
            args += ["-f", str(self.source), "-l", str(self.log), "-r", str(self.rule),
                     "-d", str(self.delay), "-w", str(self.wait)]
        process = subprocess.Popen(args, stdout=stream, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        self.processes.append(process)
        return process, output

    def __enter__(self):
        self.owner, _ = self.launch("auctioneer")
        wait_for(self.log, "Ожидание процессов:", self.owner)
        return self

    def observer(self):
        process, output = self.launch("observer")
        wait_for(output, "КОНЕЦ СНИМКА", process)
        return process, output

    def workers(self):
        self.workers_by_role = {}
        for program, count in zip(("participant", "lot"), map(int, self.data.split()[:2])):
            for index in range(1, count + 1):
                process, _ = self.launch(program, index)
                self.workers_by_role[program, index] = process

    def detach(self, process, signum=signal.SIGTERM):
        process.send_signal(signum)
        expected = -signum if signum == signal.SIGKILL else 128 + signum
        assert process.wait(timeout=5) == expected
        self.detached[process] = expected

    def finish(self, expected=0):
        global passed
        for process in self.processes:
            assert process.wait(timeout=20) == self.detached.get(process, expected), self.path
        text = self.log.read_text()
        check_log(text, self.data, self.rule)
        passed += 1
        print(f"OK: {self.name}, резервы {self.rule}", flush=True)
        return text

    def __exit__(self, *_):
        if self.owner.poll() is None:
            self.owner.terminate()
        for process in self.processes:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        for stream in self.files:
            stream.close()


def main():
    for name, rule in [("simple", 0), ("unsold", 0), ("deadline", 0), ("extension", 0),
                       ("reserves", 0), ("reserves", 1), ("refusal", 1)]:
        with Session(name, rule) as session:
            _, first = session.observer()
            _, second = session.observer()
            session.workers()
            text = session.finish()
            if name == "simple":
                assert "Лот 1: продан, победитель 1, цена 15." in text
            elif name in ("unsold", "deadline"):
                assert "Оплата:" not in text
            elif name == "extension":
                assert "Продление:" in text
            elif name == "reserves" and rule == 1:
                assert "Сохранение резерва:" in text
            elif name == "refusal":
                assert "Отказ: участник 1" in text and "Лот 1: продан, победитель 2" in text
            # Оба наблюдателя получают одинаковые события после начала торгов.
            marker = "Все процессы подключены. Начало торгов."
            assert first.read_text().split(marker)[1] == second.read_text().split(marker)[1]
            assert "=== ИТОГИ:" in first.read_text()

    data = "1 2 1 0 0\n100 0 4 0\n10 5 12\n20 5 40\n"
    with Session("reconnect", data=data, delay=40) as session:
        observer, _ = session.observer()
        session.observer()
        session.workers()
        wait_for(session.log, "Подготовка ставки:", session.owner)
        session.detach(session.workers_by_role["participant", 1])
        wait_for(session.log, "Пауза:", session.owner)
        participant, _ = session.launch("participant", 1)
        wait_for(session.log, "Принятие:", session.owner)
        assert session.log.read_text().count("Подготовка ставки:") == 1
        wait_for(session.log, "Резервирование: участник 1, лот 2", session.owner)
        session.detach(participant)
        wait_for(session.log, "Пауза:", session.owner, count=2)
        frozen = re.findall(r"=== t=(\d+) ===", session.log.read_text())[-1]
        time.sleep(0.12)
        assert re.findall(r"=== t=(\d+) ===", session.log.read_text())[-1] == frozen
        session.detach(session.workers_by_role["lot", 2], signal.SIGKILL)
        wait_for(session.log, "Отключение: лот 2", session.owner)
        session.detach(observer, signal.SIGINT)
        _, snapshot = session.observer()
        assert "Лот 1: продан" in snapshot.read_text()
        assert "потрачено 15, остаток 85, резерв 25" in snapshot.read_text()
        session.launch("participant", 1)
        session.launch("lot", 2)
        text = session.finish()
        assert "потрачено 40, остаток 60, резерв 0" in text

    with Session("paid_interrupt") as session:
        session.observer()
        session.workers()
        wait_for(session.log, "Оплата:", session.owner)
        session.owner.send_signal(signal.SIGTERM)
        text = session.finish(143)
        assert "Лот 1: продан" in text and "Лот 2: отменён" in text

    # Один запрос разбит между send, два следующих приходят вместе.
    with Session("protocol", data=Path("tests/data/simple.txt").read_text()) as session:
        with socket.create_connection(("127.0.0.1", session.port), timeout=5) as client:
            client.sendall(b"JO")
            time.sleep(0.02)
            client.sendall(b"IN 1 1\nVIEW\n")
            with client.makefile("rb") as stream:
                for _ in range(2):
                    assert stream.readline().startswith(b"STATE ")
                    while True:
                        line = stream.readline()
                        assert line, "Сервер закрыл соединение внутри снимка"
                        if line == b"END\n":
                            break
            duplicate = subprocess.run(["./participant", *session.endpoint, "-i", "1"],
                                       capture_output=True, timeout=5)
            assert duplicate.returncode == 1
        wait_for(session.log, "Отключение: участник 1", session.owner)
        session.workers()
        session.finish()

    with Session("timeout", data=Path("tests/data/simple.txt").read_text(), wait=1) as session:
        session.observer()
        session.finish(1)

    # Диапазоны аргументов и защита исходных данных от перезаписи журналом.
    for args in [["-p", "0"], ["-d", "-1"], ["-r", "2"], ["-a", "bad"],
                 ["-f", "tests/data/simple.txt", "-l", "tests/data/simple.txt"]]:
        source = Path("tests/data/simple.txt").read_bytes()
        result = subprocess.run(["./auctioneer", "-a", "127.0.0.1", "-p", "5000", *args],
                                capture_output=True, timeout=5)
        assert result.returncode == 1 and result.stderr
        assert Path("tests/data/simple.txt").read_bytes() == source
    print(f"Все {passed} сценариев и проверки ошибок пройдены.")


if __name__ == "__main__":
    main()
