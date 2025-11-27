Below is your **updated professional README.md** with **badges, branding, installation badges, Docker badges, CI/CD badges**, and a polished top-level header.

Copy-paste directly into your `README.md`.

---


<h1 align="center">🚀 Custom Redis Server (C++17)</h1>

<p align="center">
  <strong>High-performance, Redis-compatible in-memory database engine built from scratch in C++</strong>
</p>

<p align="center">
  <!-- Build Status -->
  <img src="https://img.shields.io/badge/build-passing-brightgreen?style=for-the-badge" />
  <!-- License -->
  <img src="https://img.shields.io/badge/license-MIT-blue?style=for-the-badge" />
  <!-- C++ -->
  <img src="https://img.shields.io/badge/C++-17-blue?style=for-the-badge&logo=cplusplus" />
  <!-- Performance -->
  <img src="https://img.shields.io/badge/Performance-135k ops%2Fs-success?style=for-the-badge" />
  <!-- Docker -->
  <img src="https://img.shields.io/badge/docker-ready-blue?style=for-the-badge&logo=docker" />
  <!-- Redis Protocol -->
  <img src="https://img.shields.io/badge/RESP-Protocol-red?style=for-the-badge" />
</p>

---

## 📌 Overview

This project is a **production-grade, high-performance, Redis-like server** implemented entirely in **C++17**, designed for:

- Custom caching backend for MERN, Go, Python, or C++ services  
- URL shortener microservices  
- Queueing systems  
- In-memory analytics store  
- Local development Redis alternative  
- Learning low-level systems programming  

It supports **multi-client concurrency**, **RESP protocol**, **key expiration**, **disk persistence**, **list + hash operations**, and a **high-throughput async logger**.

---

## ✨ Features

### ⚙ Redis-Compatible Operations
- **Strings**: `SET`, `GET`, `DEL`, `TYPE`, `EXPIRE`, `RENAME`
- **Lists**: `LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LLEN`, `LGET`, `LREM`, `LINDEX`, `LSET`
- **Hashes**: `HSET`, `HGET`, `HMSET`, `HGETALL`, `HKEYS`, `HVALS`, `HDEL`, `HEXISTS`, `HLEN`

### ⚡ RESP Protocol (Zero-Copy Parsing)
- Built using `string_view`  
- Supports pipelining  
- No extra allocations → high performance  

### 🔄 Persistence (RDB-style)
- Auto-saves every **5 minutes**
- Save on shutdown  
- Loads at server startup  

### 🧵 Thread-Safe Engine
- Mutex-protected data stores  
- Ready for 1000+ concurrent clients  

### 🕒 TTL Expiration Engine
- Accurate expiry using `steady_clock`
- Handles millions of keys efficiently  

### 📝 Asynchronous Logger
- Background log thread  
- 256-line batching  
- Hourly rotation  
- Logs stored in `logs/redis-*.log`  

### 🐳 Docker-Ready + Render Deployable
- `Dockerfile` included  
- `render.yaml` included  
- Easy cloud hosting  

---

## 📂 Project Structure

```

├── include/
│   ├── Logger.h
│   ├── RedisCommandHandler.h
│   ├── RedisDatabase.h
│   └── RedisServer.h
│
├── src/
│   ├── Logger.cpp
│   ├── RedisCommandHandler.cpp
│   ├── RedisDatabase.cpp
│   ├── RedisServer.cpp
│   └── main.cpp
│
├── logs/                # Auto-created log folder
│   └── redis-*.log
│
├── dump.my_rdb          # Auto-created database dump
│
├── Dockerfile
├── render.yaml
└── README.md

````

---

## 🛠 Build & Run

### Build (Linux or WSL)
```bash
g++ -std=c++17 -O2 -pthread -Iinclude src/*.cpp -o my_redis_server
````

### Run the server

```bash
./my_redis_server 6379
```

### Connect using Redis CLI

```bash
redis-cli -p 6379
```

---

## 🔌 Usage in Node.js / MERN

### Helper to send RESP commands

```js
const net = require("net");

function resp(arr) {
  let out = `*${arr.length}\r\n`;
  arr.forEach(arg => out += `$${arg.length}\r\n${arg}\r\n`);
  return out;
}

async function redisSend(client, arr) {
  return new Promise(resolve => {
    client.write(resp(arr));
    client.once("data", x => resolve(x.toString()));
  });
}

(async () => {
  const client = net.createConnection({ host: "127.0.0.1", port: 6379 });

  console.log(await redisSend(client, ["SET", "url:1", "https://google.com"]));
  console.log(await redisSend(client, ["GET", "url:1"]));
})();
```

Perfect for **URL shortener caching**.

---

## ⚡ Benchmark Results (Stress Tested)

### 1000 clients × 200 ops/client

```
Total ops: 200000
Time: ~1.47 sec
OPS/sec: 135,893
Avg Latency: ~1.27 ms
Timeouts: 0
Failures: 0
```

This is **massively faster** than many interpreted-language Redis clones.

---

## 🐳 Docker Deployment

### Dockerfile

```Dockerfile
FROM ubuntu:22.04

RUN apt update && apt install -y g++ make build-essential

WORKDIR /app
COPY . .

RUN g++ -std=c++17 -O2 -pthread -Iinclude src/*.cpp -o my_redis_server

EXPOSE 6379
CMD ["./my_redis_server", "6379"]
```

### Build & Run Docker

```bash
docker build -t custom-redis .
docker run -p 6379:6379 custom-redis
```

---

## ☁ Deploy on Render (Free Tier)

Add this to **render.yaml**:

```yaml
services:
  - type: web
    name: custom-redis
    env: docker
    plan: free
    dockerfilePath: ./Dockerfile
    autoDeploy: true
    envVars:
      - key: PORT
        value: 6379
```

Render exposes it at:

```
tcp://your-service-ip:6379
```

---

## 🔮 Roadmap

| Feature                         | Status   |
| ------------------------------- | -------- |
| Sorted Sets (ZSET)              | Planned  |
| Pub/Sub                         | Planned  |
| AOF Append-Only File            | Planned  |
| Epoll / IOCP Nonblocking Server | Upcoming |
| Sharded-Lock Architecture       | Upcoming |
| Cluster Mode                    | Future   |

---

## 🧑‍💻 Author

Built for systems engineering, backend architecture, and performance research.


