#!/bin/bash
host=localhost
port=8088
td=$(mktemp -d)

function fail {
  echo "FAIL"
  rm -rf $td
  kill -9 %1
  exit 1
}

mkdir -p $td/wwwroot
./shim -p $port -r $td/wwwroot  -f &
sleep 1


# Basic compressed stream example using default compression
s=`curl -s -k http://${host}:${port}/new_session | tr -d '[\r\n]'`
curl -f -s -k "http://${host}:${port}/execute_query?id=${s}&query=list('functions')&save=dcsv&compression=9" >/dev/null || fail
curl -f -s -k "http://${host}:${port}/read_bytes?id=${s}&n=0" >/tmp/___z.gz 2>/dev/null || fail
# Decompress
gunzip -c /tmp/___z.gz >/dev/null || fail
rm -f /tmp/___z.gz
echo "OK"

rm -rf $td
kill -9 %1 >/dev/null 2>&1
