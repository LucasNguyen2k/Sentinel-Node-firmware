"""python3 -m hw_interface   (settings from HWI_* environment variables, see config.py)"""
import logging
import signal
import threading

from .service import HwInterface


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    hw = HwInterface().start()
    done = threading.Event()
    signal.signal(signal.SIGTERM, lambda *a: done.set())
    signal.signal(signal.SIGINT, lambda *a: done.set())
    done.wait()
    hw.close()


if __name__ == "__main__":
    main()
