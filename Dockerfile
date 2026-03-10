# ── Stage 1: build ────────────────────────────────────────────────────────────
FROM gcc:13-bookworm AS builder

WORKDIR /build
COPY src/ .

RUN gcc -pthread -O2 -Wall -Wextra -Wpedantic -Wstrict-overflow \
        -fno-strict-aliasing -std=gnu11 \
        main.c fiche.c -o fiche


# ── Stage 2: runtime ──────────────────────────────────────────────────────────
FROM debian:bookworm-slim

COPY --from=builder /build/fiche          /usr/local/bin/fiche
COPY docker-entrypoint.sh                 /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

EXPOSE 9999
EXPOSE 9998

ENTRYPOINT ["docker-entrypoint.sh"]
