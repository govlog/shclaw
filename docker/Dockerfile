# shclaw — minimal runtime image
#
# Build the binary first:  make musl
# Build the image:         make docker-image
# Run:                     docker run -v /path/to/instance:/app/instance shclaw

FROM alpine:3.21

RUN apk add --no-cache ca-certificates \
    && addgroup -S shclaw && adduser -S -G shclaw -H -s /sbin/nologin shclaw

COPY shclaw /usr/local/bin/shclaw

USER shclaw

ENTRYPOINT ["shclaw", "--workdir=/app/instance"]
