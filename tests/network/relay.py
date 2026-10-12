#!/usr/bin/env python3
"""A bounded, reproducible UDP impairment relay for real game sessions.

Each relay serves one client. It pins the first client's source address and
uses a separate upstream socket, so multiple relays appear as distinct peers.
Loss is per physical datagram: three duplicate sends have three independent
opportunities to arrive. No game protocol is interpreted or acknowledged.
"""

import argparse
import collections
import hashlib
import heapq
import json
import random
import selectors
import signal
import socket
import threading
import time


class Relay:
    def __init__(self, target, *, loss=0.0, delay_ms=0.0, jitter_ms=0.0,
                 reorder=0.0, seed=1, listen_port=0, max_queued=16384):
        if not 0 <= loss <= 1 or not 0 <= reorder <= 1:
            raise ValueError("loss and reorder must be in [0, 1]")
        if delay_ms < 0 or jitter_ms < 0 or max_queued < 1:
            raise ValueError("delays must be nonnegative and queue must be positive")
        self.target = (socket.gethostbyname(target[0]), int(target[1]))
        self.loss = loss
        self.delay_ms = delay_ms
        self.jitter_ms = jitter_ms
        self.reorder = reorder
        self.seed = seed
        self.random = random.Random(seed)
        self.max_queued = max_queued
        self.client = None
        self.block_client_payload = None
        self.downstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.downstream.bind(("127.0.0.1", listen_port))
        self.upstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.upstream.bind(("127.0.0.1", 0))
        self.selector = selectors.DefaultSelector()
        for stream, direction in ((self.downstream, "client_to_server"),
                                  (self.upstream, "server_to_client")):
            # Fragmented snapshots are sent in bursts, amplified by packet
            # copies; don't turn the relay's own socket buffer into hidden loss.
            stream.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4*1024*1024)
            stream.setblocking(False)
            self.selector.register(stream, selectors.EVENT_READ, direction)
        self.port = self.downstream.getsockname()[1]
        self.queue = []
        self.sequence = 0
        self.stopping = threading.Event()
        self.thread = None
        self.failure = None
        self.counters = collections.Counter()
        self.fingerprints = collections.Counter()
        self.last_delivered = {}
        self.started = time.monotonic()

    def start(self):
        self.thread = threading.Thread(target=self.run, name="udp-relay", daemon=True)
        self.thread.start()
        return self

    def _receive(self, stream, direction):
        # Bound each poll's work so a busy sender cannot starve queued delivery.
        for _ in range(256):
            try:
                data, source = stream.recvfrom(65536)
            except BlockingIOError:
                return
            if direction == "client_to_server":
                if self.client is None:
                    self.client = source
                if source != self.client:
                    self.counters["foreign_source"] += 1
                    continue
                destination, outbound = self.target, self.upstream
            else:
                if source != self.target or self.client is None:
                    self.counters["foreign_source"] += 1
                    continue
                destination, outbound = self.client, self.downstream
            self.counters[direction + "_received"] += 1
            self.counters[direction + "_bytes"] += len(data)
            # Retain bounded fingerprints, not unbounded captured game data.
            fingerprint = (direction, hashlib.sha256(data).hexdigest())
            if len(self.fingerprints) < 65536 or fingerprint in self.fingerprints:
                self.fingerprints[fingerprint] += 1
            if (direction == "client_to_server" and self.block_client_payload is not None
                    and self.block_client_payload in data):
                self.counters["client_to_server_filtered"] += 1
                continue
            if self.random.random() < self.loss:
                self.counters[direction + "_lost"] += 1
                continue
            if len(self.queue) >= self.max_queued:
                self.counters["queue_overflow"] += 1
                continue
            delay = max(0.0, self.delay_ms + self.random.uniform(-self.jitter_ms, self.jitter_ms))
            if self.random.random() < self.reorder:
                delay += max(1.0, self.delay_ms + 2.0 * self.jitter_ms)
                self.counters[direction + "_extra_delayed"] += 1
            self.sequence += 1
            heapq.heappush(self.queue, (time.monotonic() + delay / 1000,
                                      self.sequence, outbound, destination, data, direction))
            self.counters["peak_queued"] = max(self.counters["peak_queued"], len(self.queue))

    def _deliver(self):
        now = time.monotonic()
        while self.queue and self.queue[0][0] <= now:
            _, sequence, outbound, destination, data, direction = heapq.heappop(self.queue)
            try:
                outbound.sendto(data, destination)
            except BlockingIOError:
                self.counters["socket_backpressure_drop"] += 1
                continue
            self.counters[direction + "_delivered"] += 1
            if sequence < self.last_delivered.get(direction, 0):
                self.counters[direction + "_reordered"] += 1
            self.last_delivered[direction] = sequence

    def run(self):
        try:
            while not self.stopping.is_set():
                self._deliver()
                timeout = min(0.02, max(0, self.queue[0][0] - time.monotonic())) if self.queue else 0.02
                for key, _ in self.selector.select(timeout):
                    self._receive(key.fileobj, key.data)
        except Exception as error:  # Surface thread failures to the test process.
            self.failure = error
            self.stopping.set()

    def close(self):
        self.stopping.set()
        if self.thread:
            self.thread.join(timeout=5)
            if self.thread.is_alive():
                raise RuntimeError("UDP relay failed to stop")
        self.selector.close()
        self.downstream.close()
        self.upstream.close()
        if self.failure:
            raise RuntimeError("UDP relay failed") from self.failure

    def report(self):
        multiplicities = {}
        for direction in ("client_to_server", "server_to_client"):
            histogram = collections.Counter(count for (kind, _), count in self.fingerprints.items()
                                            if kind == direction)
            multiplicities[direction] = dict(sorted(histogram.items()))
        return {"seed": self.seed, "loss": self.loss, "delay_ms": self.delay_ms,
                "jitter_ms": self.jitter_ms, "reorder_probability": self.reorder,
                "counters": dict(self.counters), "identical_datagram_multiplicity": multiplicities,
                "remaining_queued": len(self.queue), "elapsed_seconds": time.monotonic() - self.started}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", default="127.0.0.1")
    parser.add_argument("--target-port", required=True, type=int)
    parser.add_argument("--port", default=0, type=int)
    parser.add_argument("--loss", default=0.0, type=float)
    parser.add_argument("--delay-ms", default=0.0, type=float)
    parser.add_argument("--jitter-ms", default=0.0, type=float)
    parser.add_argument("--reorder", default=0.0, type=float)
    parser.add_argument("--seed", default=1, type=int)
    parser.add_argument("--report", required=True)
    args = parser.parse_args()
    relay = Relay((args.target, args.target_port), loss=args.loss, delay_ms=args.delay_ms,
                  jitter_ms=args.jitter_ms, reorder=args.reorder, seed=args.seed, listen_port=args.port)
    for stop_signal in (signal.SIGINT, signal.SIGTERM):
        signal.signal(stop_signal, lambda *_: relay.stopping.set())
    print(f"UDP relay listening on 127.0.0.1:{relay.port}", flush=True)
    try:
        relay.run()
    finally:
        relay.close()
        with open(args.report, "w") as output:
            json.dump(relay.report(), output, indent=2)
            output.write("\n")


if __name__ == "__main__":
    main()
