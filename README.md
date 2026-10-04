# Termbin Reborn (fiche)

A patched, self-hostable Docker stack built on [fiche/termbin](https://github.com/solusipse/fiche), the minimal TCP pastebin (`cat file | nc domain 9999`).

This fork fixes five bugs present in the upstream C source — including a predictable-RNG issue affecting slug and delete-token security — adds self-service **slug deletion**, and hardens the container (non-root, dropped capabilities, read-only rootfs).

<img width="1920" height="918" alt="immagine" src="https://github.com/user-attachments/assets/3e7d3311-48c2-4de4-ac45-db4278f606ba" />


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

Because the container runs as the non-root `fiche` user, the host `./data`
directory must be writable by that user. If you hit "permission denied" on the
volume, align ownership to the container's UID, e.g.:

```bash
# UID of the fiche user inside the image (check with: docker run --rm <img> id -u fiche)
sudo chown -R 100999:100999 ./data   # example UID; use the one your image reports
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

### 5 — Predictable slugs and delete tokens

Slugs and delete tokens were generated with `rand_r()` seeded once by `time(NULL)`: a 32-bit, guessable-at-startup seed, shared across all worker threads without locking (a data race). Because slugs are public — they live in the paste URL — an attacker who knows the approximate start time can brute-force the seed and derive the delete token of any paste, then delete it.

**Fix:** both slugs and tokens now come from `getrandom(2)` (with a `/dev/urandom` fallback), with rejection sampling to remove modulo bias. Generation failure is treated as fatal for that request instead of emitting a weak value. The shared `seed` global and `rand_r` are gone.

---

## Hardening

- **Non-root container.** The image runs as an unprivileged `fiche` user. fiche binds ports > 1024, so root is unnecessary; this contains the blast radius of any memory-safety bug in the C parser.
- **`docker-compose` builds from source** (`build: .`) and tags the image, so the running binary matches this repository rather than an opaque published image.
- **Reduced capabilities.** The fiche service runs with `cap_drop: ALL`, `no-new-privileges`, and a `read_only` root filesystem (only the `/data` bind-mount is writable).
- **`.env` is not committed.** Copy `.env.example` to `.env` and edit it; `.env`, `data/` and `*.log` are git-ignored.

---

## Source

- Patched source and full Docker stack: https://github.com/Leproide/termbin-reborn
- Upstream original: https://github.com/solusipse/fiche
- Docker HUB: https://hub.docker.com/r/leprechaunit/fiche
<<<<<<< HEAD
=======

---

## License

This project is distributed under the **GNU General Public License v3.0**
(GPL-3.0); see the `LICENSE` file.

It is a derivative of [fiche](https://github.com/solusipse/fiche) by solusipse,
originally licensed under the MIT License. The original MIT copyright notice is
retained in `LICENSE`, as the MIT terms require. MIT permits redistribution of
derivatives under the GPL-3.0.

## Author

Patches and Docker stack: [https://github.com/Leproide](https://github.com/Leproide)
>>>>>>> 5915718 (fix(security): replace predictable RNG; harden container and licensing)
