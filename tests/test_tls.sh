#!/bin/bash
host=localhost
port=8083
td=$(mktemp -d)

mkdir -p $td/wwwroot
openssl req -new -newkey rsa:4096 -days 3650 -nodes -x509 -subj "/C=US/ST=MA/L=Waltham/O=Paradigm4/CN=$(hostname)" -keyout $td/ssl_cert.pem 2>/dev/null >> $td/ssl_cert.pem
./shim -p ${port}s -r $td/wwwroot  -f &
sleep 1

curl s -k "https://${host}:${port}/version"

rm -rf $td
kill -9 %1
