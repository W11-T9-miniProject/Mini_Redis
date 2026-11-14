# Mini Redis

A small, in-memory TCP key/value server that speaks the Redis RESP protocol. It uses only Node.js built-ins and is intended for learning and local experiments; data is lost when the process stops.

## Run

```sh
cd mini-redis
npm start
```

The server listens on `127.0.0.1:6379` by default. Set `HOST` or `PORT` to change the bind address. Connect with `redis-cli -h 127.0.0.1 -p 6379`.

## Commands

- `PING [message]`
- `SET key value [EX seconds | PX milliseconds]`
- `GET key`
- `DEL key [key ...]`
- `EXISTS key [key ...]`
- `EXPIRE key seconds`
- `TTL key` (`-2` missing, `-1` no expiry)

Each client connection supports multiple RESP commands, including pipelined requests. Values are UTF-8 strings. The implementation is single-process and in-memory; persistence, authentication, and Redis data structures are not included.
