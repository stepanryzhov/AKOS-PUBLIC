#!/usr/bin/env python3
"""Локальный запуск всех ролей; в терминале показывается вывод наблюдателя."""
from pathlib import Path
import signal
import subprocess
import sys
import time


def main():
    address, port, source, delay, rule, log, wait = sys.argv[1:]
    participants, lots = map(int, Path(source).read_text().split()[:2])
    endpoint = ["-a", address, "-p", port]
    processes = []
    owner = subprocess.Popen(
        ["./auctioneer", *endpoint, "-f", source, "-d", delay, "-r", rule,
         "-l", log, "-w", wait], stdout=subprocess.DEVNULL, start_new_session=True)
    processes.append(owner)
    try:
        deadline = time.monotonic() + 5
        while True:
            if owner.poll() is not None:
                return owner.returncode
            # Проверяем готовность именно своего сервера, не подключаясь к чужому порту.
            if Path(log).exists() and f"PID {owner.pid}." in Path(log).read_text():
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("Сервер не начал принимать подключения")
            time.sleep(0.02)
        observer = subprocess.Popen(["./observer", *endpoint], start_new_session=True)
        processes.append(observer)
        for program, count in [("participant", participants), ("lot", lots)]:
            for index in range(1, count + 1):
                processes.append(subprocess.Popen(
                    ["./" + program, *endpoint, "-i", str(index)],
                    stdout=subprocess.DEVNULL, start_new_session=True))
        status = owner.wait()
        return status if observer.wait(timeout=5) == status else 1
    except KeyboardInterrupt:
        owner.send_signal(signal.SIGINT)
        owner.wait(timeout=5)
        return 130
    finally:
        if owner.poll() is None:
            owner.terminate()
        for process in processes:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    sys.exit(main())
