# B-Tree Key-Value Server — Wire Protocol

This document defines the application-level protocol used between clients and the
single-threaded, `select()`-based TCP server (see [src/btree_server.cpp](../src/btree_server.cpp)).

## Command Format

* The protocol is **line based / text based**. Every request and every response is
  a single line terminated by `\n`.
* keywords/parameters within a line are separated by a single space (`' '`).
* Keys and values **must not contain whitespace** and must
  be at most **64 bytes**. Longer keys/values are rejected with `ERR PROTOCOL`.

## Requests (client → server)

| Command | Meaning |
|---|---|
| `PUT <key> <value>`      | Write a key/value pair. |
| `GET <key>`              | Read a value. |
| `CONTAINS <key>`         | Test for key presence. |
| `BEGIN <key1> [key2 ...]`| Start a transaction, locking the listed keys. |
| `COMMIT`                 | Commit the active transaction. |
| `ABORT`                  | Abort the active transaction. |
| `SHUTDOWN`               | Begin a clean shutdown of the server. |

`PUT`, `GET` and `CONTAINS` may be issued **outside** a transaction (each is an
independent, immediately-durable operation) or **inside** a transaction (buffered
/ served from the in-memory buffer). Inside a transaction they may only reference
keys that were locked by the enclosing `BEGIN`.

## Responses (server → client)

Every request produces exactly one response line.

| Response | Meaning |
|---|---|
| `OK`                       | Generic success (e.g. `COMMIT`, `ABORT`). |
| `OK <old-value>`           | `PUT` succeeded; previous value returned. |
| `OK NULL`                  | `PUT` succeeded; key had no previous value. |
| `OK <expiration-ms>`       | `BEGIN` succeeded; value is the transaction expiration time (epoch milliseconds). |
| `VALUE <value>`            | `GET` found a value. |
| `NULL`                     | `GET` found no value. |
| `TRUE` / `FALSE`           | `CONTAINS` result. |
| `BYE`                      | Sent just before the server closes the connection during shutdown. |
| `ERR <code>`               | The request failed; see error codes below. |

### Error codes

| Code | Meaning |
|---|---|
| `KEY_LOCKED`     | The key is locked by another client's transaction. |
| `TXN_EXPIRED`    | The active transaction has expired; the client must `ABORT`. |
| `LOCK_FAILED`    | `BEGIN` could not acquire locks on all requested keys (none were taken). |
| `NO_WRITES`      | `COMMIT` issued with an empty write buffer; the client must `ABORT`. |
| `NO_TXN`         | `COMMIT`/`ABORT`/txn-operation issued with no active transaction. |
| `TXN_ACTIVE`     | `BEGIN` issued while a transaction is already active. |
| `KEY_NOT_LOCKED` | A transactional `PUT`/`GET`/`CONTAINS` referenced a key not locked by `BEGIN`. |
| `SHUTTING_DOWN`  | The server is shutting down and is not accepting new work. |
| `PROTOCOL`       | The request was malformed (unknown command, oversized token). |

## Transaction lifecycle

```
BEGIN alpha beta gamma      -> OK 1751690000000
PUT alpha 0                 -> OK NULL          (buffered)
PUT beta 1                  -> OK NULL          (buffered)
PUT alpha 1                 -> OK 0             (buffered, previous buffered value)
GET gamma                   -> NULL             (buffered miss -> B-tree miss)
COMMIT                      -> OK               (buffered writes flushed to B-tree)
```

Read-only transaction:

```
BEGIN alpha beta            -> OK 1751690000000
GET alpha                   -> VALUE 1
GET beta                    -> VALUE 1
ABORT                       -> OK               (COMMIT would return ERR NO_WRITES)
```

### Expiration

Each transaction expires `TXN_TIMEOUT` (default 30s) after `BEGIN`. After
expiry, every transactional operation and `COMMIT` returns `ERR TXN_EXPIRED`.
The owning client should issue `ABORT` (which always succeeds) to release its
locks.

An expired transaction's locks are also reclaimed **lazily**: when another
client attempts to `BEGIN`, `PUT`, `GET`, or `CONTAINS` a key held by an expired
transaction, the server aborts the expired transaction (releasing all of its
locks) and allows the request to proceed. This prevents an abandoned client from
holding keys indefinitely. Once reclaimed, the original owner is no longer in a
transaction, so its subsequent `ABORT` returns `ERR NO_TXN`.

## Durability & the write-ahead log (WAL)

The server keeps a write-ahead log (see [include/wal.h](../include/wal.h)). The
WAL is an append-only text file with one record per line:

```
BEGIN  <txn-id> <key1> <key2> ...
WRITE  <txn-id> <key> <value>
COMMIT <txn-id>
ABORT  <txn-id>
```

### Recovery

On startup, before accepting connections, the server replays the WAL:

* For every transaction that has a `COMMIT` record, all of its `WRITE` records
  are re-applied to the B-tree in order.
* Transactions without a `COMMIT` record are ignored.
