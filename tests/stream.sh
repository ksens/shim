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

s=`curl -s -k http://${host}:${port}/new_session | tr -d '[\r\n]'`
curl -f -s -k "http://${host}:${port}/execute_query?id=${s}&query=list('functions')&save=dcsv&stream=1" >/dev/null || fail
curl -s -k "http://${host}:${port}/read_bytes?id=${s}&n=0" >/dev/null || fail
echo "OK"

rm -rf $td
kill -9 %1 >/dev/null 2>&1
