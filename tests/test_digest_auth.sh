#!/bin/bash
# This test requires that the shim wwwroot directory contain an .htpasswd file
# with the line:
# homer:elmo

if test -z "${host}"; then
  host=localhost
fi

if test -z "${port}"; then
  port=8080
fi

curl --verbose --digest --user homer:elmo "http://${host}:${port}/new_session"
id=1
curl --verbose  --digest --user homer:elmo "http://${host}:${port}/execute_query?id=${id}&query=list('functions')&save=dcsv"

curl -s --digest --user homer:elmo "http://${host}:${port}/read_lines?id=${id}&n=0"
curl -s --digest --user homer:elmo "http://${host}:${port}/release_session?id=${id}"
