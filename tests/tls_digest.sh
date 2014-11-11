#!/bin/bash

host=localhost
port=8080
td=$(mktemp -d)

mkdir -p $td/wwwroot
# Add the digest user name and password
echo "homer:elmo" > $td/wwwroot/.htpasswd
# Add a tls cert
openssl req -new -newkey rsa:4096 -days 3650 -nodes -x509 -subj "/C=US/ST=MA/L=Waltham/O=Paradigm4/CN=$(hostname)" -keyout $td/ssl_cert.pem 2>/dev/null >> $td/ssl_cert.pem
./shim -p ${port}s -r $td/wwwroot  -f &
sleep 1

curl --verbose -k --digest --user homer:elmo "https://${host}:${port}/new_session"
id=1
curl --verbose -k --digest --user homer:elmo "https://${host}:${port}/execute_query?id=${id}&query=list('functions')&save=dcsv"

curl -s -k --digest --user homer:elmo "https://${host}:${port}/read_lines?id=${id}&n=0"
curl -s -k --digest --user homer:elmo "https://${host}:${port}/release_session?id=${id}"

killall shim
rm -rf $td
