# Termbin Reborn (fiche)

A patched, production-ready Docker image of [fiche/termbin](https://github.com/solusipse/fiche), the minimal TCP pastebin (`cat file | nc domain 9999`).

This fork fixes four silent bugs present in the upstream C source and adds self-service **slug deletion** support.

<img width="1920" height="927" alt="immagine" src="https://github.com/user-attachments/assets/69b68143-371d-4667-a76d-aa39474dd21b" />


---

## Quick start

```bash
cat file | nc your.domain 9999
# → https://your.domain/a3f9b2c1d4e5f6a7
```

---

## Full stack (fiche + nginx)

The recommended setup runs two containers:

- **fiche** — accepts TCP uploads, writes slugs to disk, handles deletions
- **termbin-web** — nginx serves pastes as plain text over HTTP (put your reverse proxy in front)

### 1. Get the files

```bash
git clone https://github.com/Leproide/termbin-reborn
```

### 2. Edit `.env`

```env
# Public domain returned in URLs
FICHE_DOMAIN=your.domain

# TCP port for uploads
FICHE_PORT=9999

# fiche flags (see below for reference)
FICHE_ARGS=-S -s 16 -B 10485760 -l /data/log/fiche.log -P 9998

# TCP port for delete requests
FICHE_DELETE_PORT=9998

# nginx internal port (loopback only — put a reverse proxy in front)
NGINX_PORT=62365
```

### 3. Customise the home page

`index.html` in the repo root is served at `https://your.domain/` by nginx. Edit it to add a description, usage instructions, or branding for your instance. It is mounted as a read-only bind mount and takes effect without rebuilding the image:

```yaml
volumes:
  - "./index.html:/srv/static/index.html:ro"
```

### 4. Start

```bash
docker compose up -d
```

### What `docker-compose.yml` does

```yaml
services:

  # fiche: TCP pastebin 
  fiche:
    image: leprechaunit/fiche:latest
    container_name: fiche
    restart: unless-stopped
    ports:
      - "${FICHE_PORT:-9999}:9999"          # upload
      - "${FICHE_DELETE_PORT:-9998}:9998"   # delete: echo -e "slug\ntoken" | nc domain 9998
    environment:
      FICHE_DOMAIN: ${FICHE_DOMAIN}
      FICHE_ARGS:   ${FICHE_ARGS}
    volumes:
      - "./data/paste:/data/paste"
      - "./data/log:/data/log"

  # termbin-web: nginx static file server
  termbin-web:
    image: nginx:alpine
    container_name: termbin-web
    restart: unless-stopped
    ports:
      - "127.0.0.1:${NGINX_PORT:-62365}:80"
    volumes:
      - "./data/paste:/srv/paste:ro"
      - "./nginx.conf:/etc/nginx/nginx.conf:ro"
      - "./index.html:/srv/static/index.html:ro"
```

The two containers share `./data/paste` — fiche writes, nginx reads. The nginx port is bound to `127.0.0.1` only; a reverse proxy (Caddy, nginx, Apache…) on the host should front it and terminate TLS.

---

## Environment variables

| Variable            | Default      | Description                                        |
| ------------------- | ------------ | -------------------------------------------------- |
| `FICHE_DOMAIN`      | *(required)* | Public hostname returned in uploaded URLs          |
| `FICHE_PORT`        | `9999`       | Host port mapped to the upload listener            |
| `FICHE_DELETE_PORT` | `9998`       | Host port mapped to the delete listener            |
| `FICHE_ARGS`        | *(none)*     | Extra flags passed directly to `fiche` (see below) |
| `NGINX_PORT`        | `62365`      | Internal nginx port, bound to `127.0.0.1`          |

### `FICHE_ARGS` reference

| Flag         | Description                                                   |
| ------------ | ------------------------------------------------------------- |
| `-S`         | Return `https://` links instead of `http://`                  |
| `-s <n>`     | Slug length (default: 4)                                      |
| `-B <bytes>` | Max upload size in bytes (default: 32768; 10 MB = `10485760`) |
| `-l <path>`  | Log file path inside the container                            |
| `-L <addr>`  | Listen address (default: `0.0.0.0`)                           |
| `-p <port>`  | Listen port (default: 9999)                                   |
| `-u <user>`  | Drop privileges to this user after bind                       |
| `-P <port>`  | Delete service port (`0` = disabled)                          |

---

## Slug deletion

When `-P <port>` is set, every upload returns both the paste URL and a ready-to-run delete command:

```
$ cat file.txt | nc your.domain 9999
https://your.domain/a3f9b2c1d4e5f6a7
To delete this paste:
echo -e "a3f9b2c1d4e5f6a7\n<token>" | nc your.domain 9998
```

Copy and run that line to permanently remove the paste. The token is generated once at upload time and stored server-side, there is no way to retrieve it afterwards, so save the delete command if you need it later.

---

## Volumes

| Container path | Purpose                              |
| -------------- | ------------------------------------ |
| `/data/paste`  | Slug directories (shared with nginx) |
| `/data/log`    | fiche log file                       |

Mount both with bind mounts so data survives container restarts:

```yaml
volumes:
  - "./data/paste:/data/paste"
  - "./data/log:/data/log"
```

---

## Exposed ports

| Port   | Protocol | Purpose |
| ------ | -------- | ------- |
| `9999` | TCP      | Upload  |
| `9998` | TCP      | Delete  |

---

## Bug fixes over upstream

### 1 — Stack overflow with large `-B` values

The receive buffer was a VLA on the thread stack. Any `-B` value larger than the default 32 KB caused an immediate silent stack overflow: the thread died, no URL was returned, no file was saved.

**Fix:** heap allocation via `malloc`, grown dynamically per connection.

### 2 — Connections silently dropped

`MSG_WAITALL` combined with `SO_RCVTIMEO` caused `recv` to return `≤ 0` when the client sent fewer bytes than `buffer_len` and closed the connection — the normal case for `cat file | nc`. All data was discarded silently.

**Fix:** replaced with a read loop that accumulates data until EOF or the buffer limit is reached.

### 3 — Fixed RAM reservation per connection

The original approach called `calloc(buffer_len)` for every connection regardless of upload size. A 5-byte paste with `-B 10485760` would allocate and hold 10 MB per thread.

**Fix:** buffer starts at 64 KB and grows via `realloc` only as needed up to `-B`. `malloc_trim(0)` is called after each connection to return freed heap memory to the OS immediately.

### 4 — Use-after-free in error path

`free(c)` was called before `close(c->socket)`, reading a freed struct field — undefined behaviour flagged by `-Wuse-after-free`.

**Fix:** reordered cleanup so the socket is closed before the struct is freed.

---

## Source

- Patched source and full Docker stack: https://github.com/Leproide/termbin-reborn
- Upstream original: https://github.com/solusipse/fiche
