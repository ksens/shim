#!/bin/bash
if test -z "${host}"; then
  host=localhost
fi

if test -z "${port}"; then
  port=8080
fi

id=$(wget -O - -q "${host}:${port}//new_session")
echo "New session id: $id"

qid=$(wget -O - -q "${host}:${port}//execute_query?id=${id}&query=list('functions')&save=dcsv")
echo "Query id: $qid"

wget -O - -q "${host}:${port}//read_lines?id=${id}&n=0"
wget -O - -q "${host}:${port}//release_session?id=${id}"
