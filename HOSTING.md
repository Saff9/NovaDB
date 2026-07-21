# Hosting NovaDB — What You Need

## The Short Answer

```bash
# 1. A Linux machine (any distro, any VPS, any bare-metal)
# 2. A C compiler
# 3. That's it.

git clone https://github.com/Saff9/NovaDB.git
cd NovaDB
make release
./bin/novadb-server --data-dir ./mydata --port 9876
```

NovaDB is a single static binary. You copy it to a server and run it. No Docker required (though it works fine in one). No systemd unit required (though you can write one in 30 seconds). No package manager, no apt-get, no brew. Just the binary and a directory for data.

---

## Hardware Requirements

| Environment | RAM | Disk | What you can do |
|-------------|-----|------|-----------------|
| Dev / testing | 64 MB | 10 MB | Full SQL, small datasets |
| Light production | 256 MB | 1 GB | Moderate traffic, thousands of rows |
| Serious use | 1 GB+ | 10 GB+ | Heavy read/write, many connections |

The buffer pool defaults to 4,096 pages x 8KB = ~32 MB in memory. You can tune this by editing `nvdb_config.h` before building.

Disk usage: every insert adds a B+Tree entry + a WAL record. Figure ~2x raw data size at steady state. The WAL truncates after checkpoints.

---

## Supported Platforms

| Platform | Status | Notes |
|----------|--------|-------|
| Linux x86_64 | Primary | epoll, tested |
| Linux ARM64 | Works | Raspberry Pi, AWS Graviton |
| macOS | Works | Uses poll(2) fallback |
| FreeBSD | Works | poll(2) fallback |
| Windows | Not yet | WSL or Cygwin works |

---

## Deployment Models

### Option 1: Bare binary

```bash
make release
scp bin/novadb-server user@host:/opt/novadb/
mkdir -p /var/lib/novadb
/opt/novadb/novadb-server --data-dir /var/lib/novadb --port 9876
```

Keep it running with tmux, screen, or a systemd unit.

### Option 2: systemd (production)

Create `/etc/systemd/system/novadb.service`:

```ini
[Unit]
Description=NovaDB Database Server
After=network.target

[Service]
Type=simple
User=novadb
Group=novadb
ExecStart=/opt/novadb/novadb-server --data-dir /var/lib/novadb --port 9876
Restart=on-failure
RestartSec=5
LimitNOFILE=65536
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
ReadWritePaths=/var/lib/novadb
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
```

```bash
sudo useradd -r -s /bin/false novadb
sudo mkdir -p /var/lib/novadb
sudo chown novadb:novadb /var/lib/novadb
sudo systemctl daemon-reload
sudo systemctl enable --now novadb
```

### Option 3: Docker

```dockerfile
FROM alpine:3.20
RUN apk add --no-cache build-base
COPY . /src
WORKDIR /src
RUN make release && mkdir -p /data
EXPOSE 9876
CMD ["./bin/novadb-server", "--data-dir", "/data", "--port", "9876"]
```

```bash
docker build -t novadb .
docker run -d -p 9876:9876 -v /var/lib/novadb:/data --name novadb novadb
```

### Option 4: Embedded library

```c
#include "novadb/nvdb.h"
int main(void) {
    NVDBEngine *db = nvdb_open("./data");
    nvdb_exec(db, "CREATE TABLE t (id INT, msg TEXT)");
    nvdb_close(db);
}
```

```bash
# Link everything directly:
cc -Iinclude -Isrc your_app.c src/engine.c src/common/*.c \
   src/storage/*.c src/txn/*.c src/sql/*.c \
   src/network/*.c -lpthread -lm -o your_app
```

---

## Connecting

```bash
# Built-in CLI
./bin/novadb-cli -h 127.0.0.1 -p 9876

# netcat
echo -e "QRY 18\r\nSELECT * FROM users" | nc localhost 9876

# Python
import socket
s = socket.create_connection(("localhost", 9876))
s.sendall(b"QRY 18\r\nSELECT * FROM users")
print(s.recv(4096).decode())
```

The wire protocol is plain text: `TYPE LEN\r\nPAYLOAD`. You don't need a special driver.

---

## Backups

NovaDB stores everything in two files:

```
/var/lib/novadb/
├── data.db    (B+Tree pages)
└── wal.log    (write-ahead log)
```

**Back up:**

```bash
cp -r /var/lib/novadb /backup/novadb-$(date +%Y%m%d)
```

**Restore:**

```bash
systemctl stop novadb
rm -rf /var/lib/novadb
cp -r /backup/novadb-20260721 /var/lib/novadb
systemctl start novadb
```

---

## Monitoring

| Check | Command |
|-------|---------|
| Alive? | `echo -e "PING 0\r\n" \| nc localhost 9876` |
| Disk | `du -sh /var/lib/novadb/` |
| WAL size | `ls -l /var/lib/novadb/wal.log` |
| Connections | `ss -tnp \| grep 9876 \| wc -l` |
| Memory | `ps aux \| grep novadb` |

---

## Security Notes

- No built-in auth. Bind to `127.0.0.1` and put nginx/HAProxy in front.
- No built-in TLS. Use stunnel or a TLS-terminating proxy.
- Data directory should be `chmod 0700`, owned by the novadb user.

---

## Performance Tuning

Edit `include/novadb/nvdb_config.h` before building:

| Setting | Default | Effect |
|---------|---------|--------|
| NVDB_BUFFER_POOL_SIZE | 4096 | ~32 MB cache. Raise for read-heavy workloads |
| NVDB_PAGE_SIZE | 8192 | I/O unit. 16 KB helps with larger rows |
| NVDB_MAX_CONNECTIONS | 1024 | TCP connection cap |

---

## Common Problems

**Port already in use:** `./bin/novadb-server --port 9877`

**Cannot open database:** Check `ls -la /var/lib/novadb` — the user needs read+write.

**Clients can't connect:** Check `ss -tlnp | grep 9876`. If it says `127.0.0.1`, only local connections work. Use `--host 0.0.0.0`.

**WAL file too big:** Happens if the server crashed before a checkpoint. Recovery on next start will replay and truncate.

---

## Summary

```
Needed:   Linux machine + C compiler + 5 minutes.
Binary:   Single static file. Zero dependencies.
Deploy:   scp + systemd = done.
Connect:  netcat, Python, any TCP client.
Backup:   cp the data directory.
Monitor:  PING + ps + du.
```
