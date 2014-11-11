#!/bin/bash
host=localhost
port=8080
td=$(mktemp -d)

mkdir -p $td/wwwroot
./shim -p $port -r $td/wwwroot  -f &
sleep 1


# Basic compressed stream example using default compression
s=`curl -s -k http://${host}:${port}/new_session`
echo "session: $s"
curl -s -k "http://${host}:${port}/execute_query?id=${s}&query=list('functions')&save=dcsv&compression=9"
echo
curl -s -k "http://${host}:${port}/read_bytes?id=${s}&n=0" >/tmp/z.gz
# Decompress
gunzip -c /tmp/z.gz
rm /tmp/z.gz



kill -9 %1
rm -rf $td
