#!/bin/bash

host=localhost
port=8080
td=$(mktemp -d)

mkdir -p $td/wwwroot
echo "homer:elmo" > $td/wwwroot/.htpasswd
./shim -p $port -r $td/wwwroot  -f &
sleep 1

curl --verbose --digest --user homer:elmo "http://${host}:${port}/new_session"
id=1
curl --verbose  --digest --user homer:elmo "http://${host}:${port}/execute_query?id=${id}&query=list('functions')&save=dcsv"

curl -s --digest --user homer:elmo "http://${host}:${port}/read_lines?id=${id}&n=0"
curl -s --digest --user homer:elmo "http://${host}:${port}/release_session?id=${id}"

rm -rf $td
kill -9 %1
