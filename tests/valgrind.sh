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
openssl req -new -newkey rsa:4096 -days 3650 -nodes -x509 -subj "/C=US/ST=MA/L=Waltham/O=Paradigm4/CN=$(hostname)" -keyout $td/ssl_cert.pem 2>/dev/null >> $td/ssl_cert.pem
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
echo "Compressed stream download...this can take a few minutes..."
id=$(curl -f -s --digest --user homer:elmo "http://${host}:${port}/new_session" | sed -e "s/.*//")
echo $id
curl -f -s --digest --user homer:elmo "http://${host}:${port}/execute_query?id=${id}&query=build(%3Cx%3Adouble%3E%5Bi%3D1%3A1000000%2C10000%2C0%5D%2Ci)&save=(double)&stream=1&compression=3"
curl -f -s --digest --user homer:elmo "http://${host}:${port}/read_bytes?id=${id}&n=0" | zcat | dd of=/dev/null || fail


# Uncompressed stream of binary data using TLS
echo "Uncompressed data download over TLS"
id=$(curl -k -f -s --digest --user homer:elmo "https://${host}:${port}/new_session" | sed -e "s/.*//")
echo $id
curl -k -f -s --digest --user homer:elmo "https://${host}:${port}/execute_query?id=${id}&query=build(%3Cx%3Adouble%3E%5Bi%3D1%3A1000000%2C10000%2C0%5D%2Ci)&save=(double)&stream=1"
curl -k -f -s --digest --user homer:elmo "https://${host}:${port}/read_bytes?id=${id}&n=0" | dd of=/dev/null || fail

# Data upload
echo "Uncompressed data upload (may take a while...)"
id=$(curl -f -s --digest --user homer:elmo "http://${host}:${port}/new_session" | sed -e "s/.*//")
echo $id
dd if=/dev/urandom bs=1M count=50 | curl -f -s --digest --user homer:elmo --form "fileupload=@-;filename=data" "http://${host}:${port}/upload_file?id=${id}" || fail
curl -f -s --digest --user homer:elmo "http://${host}:${port}/release_session?id=${id}" >/dev/null || fail

echo "OK"
rm -rf $td
kill -15 %1
