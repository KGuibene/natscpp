# NATS C++ Minimal Client

A tiny, dependency-free NATS client you can embed in your projects.  
Implements the essential protocol pieces:

- TCP connect → read `INFO` → send `CONNECT`
- `PING`/`PONG` keepalive
- `PUB`, `SUB`, `UNSUB`, and `MSG` dispatch
- Thread-safe publish/subscribe
- Background reader thread
- `run_forever()` to block your app until the client stops


---

## Repo layout

```
.
├─ CMakeLists.txt
├─ inc/
│  └─ nats.hpp        # public header (library API)
└─ src/
   ├─ nats.cpp        # library implementation (POSIX sockets)
   └─ main.cpp        # demo executable
```

---

## Requirements

- CMake ≥ 3.16
- C++17 compiler (g++/clang++)
- POSIX sockets (Linux/macOS/WSL)
- A running NATS server (e.g., Docker)

Start a local server:
```bash
docker run --rm -p 4222:4222 nats:latest
```

Sanity check the port:
```bash
nc 127.0.0.1 4222   # should print an INFO line immediately
```

---

## Build

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j
```

This produces:

- `libnatspp.a` (static library)
- `natspp_demo` (example executable)

---

## Quick start (demo)

```bash
./build/natspp_demo
```

What it does:

- Connects to `127.0.0.1:4222`
- Subscribes to `demo.>`
- Publishes:
  - `demo.hello` → `"hi from minimal C++"`
  - `demo.echo`  → `"ping"` with reply `_INBOX.min.1`
- Blocks with `run_forever()` until you press `Ctrl+C` (SIGINT)

Try publishing from another terminal:

```bash
# quickest: use nats-box
docker run --rm -it --network host natsio/nats-box
# inside the container:
nats sub 'demo.>'
nats pub demo.hello "hello from CLI"
```

You should see messages in your demo output.
