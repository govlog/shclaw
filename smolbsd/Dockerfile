FROM base,etc

LABEL smolbsd.service=shclaw
LABEL smolbsd.minimize=y

COPY /mnt/shclaw.com /usr/local/bin/shclaw

RUN chmod 755 /usr/local/bin/shclaw && \
    echo "nameserver 10.0.2.3" > /etc/resolv.conf && \
    cat /etc/openssl/certs/*.0 > /etc/openssl/certs/ca-certificates.crt 2>/dev/null; true

CMD ping -c1 -w2 10.0.2.3 >/dev/null 2>&1; /usr/local/bin/shclaw --workdir=/mnt
