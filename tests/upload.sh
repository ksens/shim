#!/bin/bash
# Test uploading binary data through shim

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
echo "homer:elmo" > $td/wwwroot/.htpasswd
./shim -p $port -r $td/wwwroot  -f &
sleep 1

t1=$(date +%s)
id=$(curl -f -s --digest --user homer:elmo "http://${host}:${port}/new_session" | sed -e "s/.*//")
dd if=/dev/urandom bs=1M count=50 2>/dev/null  | curl -f -s --digest --user homer:elmo --form "fileupload=@-;filename=data" "http://${host}:${port}/upload_file?id=${id}"  >/dev/null || fail
curl -f -s --digest --user homer:elmo "http://${host}:${port}/release_session?id=${id}" >/dev/null || fail
t2=$(date +%s)
x=$(( 52 / ($t2 - $t1 ) ))


echo "OK (about ${x} MB/s)"
rm -rf $td
kill -9 %1 >/dev/null 2>&1
