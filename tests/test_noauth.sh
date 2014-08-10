#!/bin/bash
host=localhost
port=8080
td=$(mktemp -d)

mkdir -p $td/wwwroot
./shim -p $port -r $td/wwwroot  -f &
sleep 1

id=$(curl -s "http://${host}:${port}/new_session")
echo "New session id: $id"

curl --verbose  "http://${host}:${port}/execute_query?id=${id}&query=list('functions')&save=dcsv"

curl -s "http://${host}:${port}/read_lines?id=${id}&n=0"
curl -s "http://${host}:${port}/release_session?id=${id}"

rm -rf $td
kill -9 %1
