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
echo "homer:elmo" > $td/wwwroot/.htpasswd
valgrind --log-file=/tmp/valgrind.out --leak-check=yes ./shim -p $port -r $td/wwwroot  -f  &
sleep 5

id=$(curl -f -s --digest --user homer:elmo "http://${host}:${port}/new_session" | sed -e "s/.*//")
curl -f -s --digest --user homer:elmo "http://${host}:${port}/execute_query?id=${id}&query=list('functions')&save=dcsv" || fail
x=1
while test $x -lt 40;do
  curl  -s --digest --user homer:elmo "http://${host}:${port}/read_bytes?id=${id}&n=555" 2>/dev/null  || break
  x=$(($x + 1))
done
curl -f -s  --digest --user homer:elmo "http://${host}:${port}/release_session?id=${id}" >/dev/null  || fail


# Basic compressed stream example using default compression
# The ugly url-encoded query string below is build(<x:double>[i=1:1000000,10000,0],i)
echo "Compressed stream test...this can take at least a few minutes."
id=$(curl -f -s --digest --user homer:elmo "http://${host}:${port}/new_session" | sed -e "s/.*//")
curl -f -s --digest --user homer:elmo "http://${host}:${port}/execute_query?id=${id}&query=build(%3Cx%3Adouble%3E%5Bi%3D1%3A1000000%2C10000%2C0%5D%2Ci)&save=(double)&stream=1&compression=9"
curl -f -s --digest --user homer:elmo "http://${host}:${port}/read_bytes?id=${id}&n=0" >/tmp/gz || fail


# Uncompressed stream of binary data
id=$(curl -f -s --digest --user homer:elmo "http://${host}:${port}/new_session" | sed -e "s/.*//")
echo $id
curl -f -s --digest --user homer:elmo "http://${host}:${port}/execute_query?id=${id}&query=build(%3Cx%3Adouble%3E%5Bi%3D1%3A1000000%2C10000%2C0%5D%2Ci)&save=(double)&stream=1"
curl -s  --digest --user homer:elmo "http://${host}:${port}/read_bytes?id=${id}&n=0" | dd of=/dev/null


echo "OK"
rm -rf $td
kill -15 %1
