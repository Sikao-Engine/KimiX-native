"""Parity tests for kimix_native.concurrency (plan 008).

Compares the native kernels against the pure-Python ``_compat`` mirrors on:
- MpscEventBus: emit 10k events -> 1 consumer receives all in order
  (drop disabled via big capacity); bounded ring (small capacity) drops
  oldest; subscribe mid-stream; unsubscribe; seq monotonicity
- IdGenerator: next() uniqueness, reserve() contiguity (native vs _compat)
- the KIMIX_NATIVE_CONCURRENCY=0 fallback toggle
"""

import os
import random
import subprocess
import sys
import threading

from kimix_native import concurrency


def test_bus_10k_events_all_received_in_order():
    native = concurrency.MpscEventBus(20000)
    compat = concurrency._CompatMpscEventBus(20000)
    for bus in (native, compat):
        sub = bus.subscribe()
        for i in range(10000):
            bus.emit(b"evt-%d" % i)
        assert bus.seq() == 10000
        got = []
        while True:
            e = bus.poll(sub)
            if e is None:
                break
            got.append(e)
        assert len(got) == 10000
        assert got == [b"evt-%d" % i for i in range(10000)]


def test_bus_bounded_drop_oldest():
    native = concurrency.MpscEventBus(4)
    compat = concurrency._CompatMpscEventBus(4)
    for bus in (native, compat):
        sub = bus.subscribe()
        for i in range(100):
            bus.emit(b"evt-%d" % i)
        got = []
        while True:
            e = bus.poll(sub)
            if e is None:
                break
            got.append(e)
        # Drop-oldest on the shared ring: only the last 4 survive.
        assert got == [b"evt-%d" % i for i in range(96, 100)]


def test_bus_subscribe_mid_stream_and_unsubscribe():
    native = concurrency.MpscEventBus(8)
    compat = concurrency._CompatMpscEventBus(8)
    for bus in (native, compat):
        bus.emit(b"old1")
        bus.emit(b"old2")
        sub = bus.subscribe()
        bus.emit(b"new1")
        assert bus.poll(sub) == b"new1"
        assert bus.poll(sub) is None
        bus.unsubscribe(sub)
        assert bus.poll(sub) is None
        # Ids are never reused.
        sub2 = bus.subscribe()
        assert sub2 != sub


def test_bus_threaded_producer_consumers():
    bus = concurrency.MpscEventBus(20000)
    subs = [bus.subscribe() for _ in range(4)]
    results = [[] for _ in range(4)]
    errors = []
    done = threading.Event()

    def consume(idx):
        try:
            while True:
                e = bus.poll(subs[idx])
                if e is None:
                    if done.is_set():
                        # Events may have arrived between this poll and the
                        # done check (the producer can finish in that window)
                        # - re-poll once before declaring caught-up.
                        e = bus.poll(subs[idx])
                        if e is None:
                            break
                    else:
                        continue
                results[idx].append(e)
        except Exception as exc:  # pragma: no cover
            errors.append(exc)

    threads = [threading.Thread(target=consume, args=(i,)) for i in range(4)]
    for t in threads:
        t.start()
    for i in range(10000):
        bus.emit(b"evt-%d" % i)
    done.set()
    for t in threads:
        t.join()
    assert not errors
    expected = [b"evt-%d" % i for i in range(10000)]
    for got in results:
        assert got == expected, (len(got), len(expected))
    assert bus.seq() == 10000


def test_id_generator_native_vs_compat():
    rng = random.Random(5)
    native = concurrency.IdGenerator(100)
    compat = concurrency._CompatIdGenerator(100)
    for _ in range(50):
        n = rng.randrange(0, 20)
        assert native.reserve(n) == compat.reserve(n)
    assert native.next() == compat.next()
    # Contiguity of reserve.
    ids = native.reserve(5)
    assert ids == list(range(ids[0], ids[0] + 5))


def test_id_generator_unique_under_threads():
    gen = concurrency.IdGenerator(0)
    results = [[] for _ in range(8)]
    threads = [threading.Thread(
        target=lambda i=i: results[i].extend(gen.reserve(2000)))
        for i in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    all_ids = [v for r in results for v in r]
    assert len(all_ids) == len(set(all_ids)) == 16000
    assert min(all_ids) == 0 and max(all_ids) == 15999


def test_native_disabled_fallback():
    env = dict(os.environ, KIMIX_NATIVE_CONCURRENCY="0")
    py_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = os.path.dirname(py_dir)
    code = (
        "import sys; sys.path.insert(0, %r); sys.path.insert(0, %r);\n"
        "from kimix_native import concurrency\n"
        "assert concurrency._USE is False\n"
        "bus = concurrency.MpscEventBus(8)\n"
        "sub = bus.subscribe()\n"
        "bus.emit(b'a'); bus.emit(b'b')\n"
        "assert bus.poll(sub) == b'a'\n"
        "assert bus.poll(sub) == b'b'\n"
        "assert bus.poll(sub) is None\n"
        "gen = concurrency.IdGenerator(3)\n"
        "assert gen.next() == 3\n"
        "assert gen.reserve(2) == [4, 5]\n"
        "print('FALLBACK_OK')\n"
    ) % (py_dir, os.path.join(root, "bin", "debug"))
    r = subprocess.run([sys.executable, "-c", code], capture_output=True,
                       text=True, env=env)
    assert r.returncode == 0, r.stderr
    assert "FALLBACK_OK" in r.stdout
