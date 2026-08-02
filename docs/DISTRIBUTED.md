# Distributed Key-Value Store

This layer turns the single-node, `select()`-based B-tree server into a
distributed store. Nothing about the server protocol changes; distribution is
implemented entirely on the **client** side (see
[include/dist_client.h](../include/dist_client.h) and
[src/dist_client.cpp](../src/dist_client.cpp)). A cluster is simply a set of
independent `db_server` processes. The distributed client is responsible for
placement, replication, and consensus.

## Concepts

### Rendezvous (highest-random-weight) hashing

Keys are placed on servers with **rendezvous hashing**
([include/rendezvous.h](../include/rendezvous.h)). For a key `k` and each server
`s`, we compute a 32-bit weight

```
weight(k, s) = MurmurHash3_x86_32(k + '\0' + s, seed = 0)
```

using the vendored, public-domain MurmurHash3 implementation
([third_party/murmur](../third_party/murmur)). Servers are ranked by descending weight (ties broken by
the server's `host:port` string), and the top **R** servers form
the key's *replica set*.

Rendezvous hashing has a useful property: **removing a server only re-homes the
keys it owned.** The relative order of the remaining servers is unchanged, so
every other key keeps its placement. This is why the client supports **server
removal** cleanly. For this implementation, we will not be supporting the addition of new servers.

### Quorum parameters

Each key is stored on `R` replicas. Reads require a read quorum `Qr` and writes
require a write quorum `Qw`. They must satisfy:

```
Qr <= R,  Qw <= R,  Qr + Qw > R,  2*Qw > R
```

* `Qr + Qw > R` guarantees every read quorum overlaps every write quorum, so a
  read always sees the latest committed write.
* `2*Qw > R` guarantees any two write quorums overlap, preventing two conflicting
  writes from both "winning".

If `Qr`/`Qw` are not supplied, the client defaults to a majority quorum:

```
Qw = floor(R/2) + 1
Qr = R - Qw + 1
```

For `R = 3` this yields `Qw = 2`, `Qr = 2`.

### Quorum reads

`GET` is sent to all `R` replicas. Responses are tallied into buckets: each
distinct `VALUE v` is a bucket, and an authoritative `NULL` (key absent) is its
own bucket. Dead servers, timeouts, and error responses contribute no vote.

* If some bucket reaches `Qr` votes, that answer is the consensus (`OK <value>`
  or `NOT_FOUND`). The client stops early as soon as a quorum is reached.
* If no bucket reaches `Qr`, the replicas disagree or too few answered:
  the read is reported as **INCONSISTENT**.

### Two-phase commit writes

`PUT` is a two-phase commit over the key's replica set, mapped onto the existing
transaction protocol (`BEGIN` locks the key, `PUT` buffers the write, `COMMIT`
flushes it):

1. **Prepare.** Send `BEGIN <key>` then `PUT <key> <value>` to each replica.
   A replica is *prepared* when both return `OK`.
   * If fewer than `Qw` replicas prepare, send `ABORT` to every prepared replica
     and report **FAILED (store consistent, no change)** — nothing was committed.
2. **Commit.** Send `COMMIT` to every prepared replica.
   * If at least `Qw` commits succeed, report **COMMITTED**.
   * If some commits succeed but fewer than `Qw`, replicas have diverged and the
     write is reported as **INCONSISTENT**.

### Timeouts

Every replica connection uses a non-blocking socket driven by `poll()` with a
per-request deadline (default 2000 ms, `--timeout`). A dead or unresponsive
server can therefore never wedge the client; it simply contributes no vote / is
treated as not prepared.

## Command-line client

Build produces `dist_db_client`. The replica-set size `R` is the first
positional argument, followed by the server endpoints:

```
./dist_db_client <R> <host:port> [host:port ...] [--qr N] [--qw N] [--timeout MS]
```

Example — three servers, `R = 3`:

```
./dist_db_client 3 127.0.0.1:25120 127.0.0.1:25121 127.0.0.1:25122
```

It reads commands from stdin, one per line:

| Command | Meaning |
|---|---|
| `PUT <key> <value>` | Quorum-replicated write (2PC). |
| `GET <key>`         | Quorum read. |
| `INFO`              | Show `R`, `Qr`, `Qw`, and reachable server count. |
| `QUIT`              | Exit. |

Sample session with one server down (`R=3`, `Qr=Qw=2`):

```
COMMITTED (prepared=2, committed=2, Qw=2)          # PUT still succeeds
OK 200 (agree=2/2, responses=2)                    # GET still succeeds
```

And with two servers down (quorum lost):

```
FAILED (store consistent, no change) (prepared=1, committed=0, Qw=2)
INCONSISTENT (agree=1/2, responses=1)
```

## Running a cluster

Start `R` (or more) independent servers, each with its own database and WAL:

```
./db_server -p 25120 -d n0.db -w n0.wal &
./db_server -p 25121 -d n1.db -w n1.wal &
./db_server -p 25122 -d n2.db -w n2.wal &
./dist_db_client 3 127.0.0.1:25120 127.0.0.1:25121 127.0.0.1:25122
```

## Tests

* [tests/rendezvous_test.cpp](../tests/rendezvous_test.cpp) — placement size,
  determinism, distribution, and the "removal only re-homes affected keys"
  property.
* [tests/dist_client_test.cpp](../tests/dist_client_test.cpp) — default quorum
  derivation, quorum-parameter validation, round-trip put/get, consistent
  not-found, survival of one dead replica, write abort when the write quorum is
  unreachable, and inconsistent-read detection across divergent replicas. Each
  test runs several real in-process servers.