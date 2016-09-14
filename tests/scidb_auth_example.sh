#!/bin/bash

# Example illustrating SciDB authentication over TLS using the default user
# name 'root' and password 'Paradigm4'. We don't run this as an automated test,
# this is just an example that shows how to do this.

# urlencode(base64(sha512(Paradigm4))):
PASSWORD=eUCUk3B57IVO9ZfJB6CIEHl%2f0lxrWg%2f7PV8KytUNY6kPLhTX2db48GHGHoizKyH%2buGkCfNTYZrJgKzjWOhjuvg%3d%3d
USER=root
host=localhost
port=8083

id=$(curl -s -k "https://${host}:${port}/new_session")
curl -f -s -k "https://${host}:${port}/execute_query?id=${id}&query=list('functions')&save=dcsv&user=${USER}&password=${PASSWORD}"
curl -f -s -k "https://${host}:${port}/read_lines?id=${id}&n=0"
curl -f -s -k "https://${host}:${port}/release_session?id=${id}"
echo

# Test the new prefix option to issue multiple commands in a single connection
# A namespace example. Part 1: Create namespace 'cazart'
id=$(curl -s -k "https://${host}:${port}/new_session")
curl -f -s -k "https://${host}:${port}/execute_query?id=${id}&query=create_namespace('cazart')&user=${USER}&password=${PASSWORD}&release=1"
echo "create_namespace('cazart')"

# Add an array to the cazart namespace
id=$(curl -s -k "https://${host}:${port}/new_session")
curl -f -s -k "https://${host}:${port}/execute_query?id=${id}&prefix=set_namespace('cazart')&query=store(list(),yikes)&user=${USER}&password=${PASSWORD}&release=1"

# context (list the contents of the 'cazart' namespace)
id=$(curl -s -k "https://${host}:${port}/new_session")
curl -f -s -k "https://${host}:${port}/execute_query?id=${id}&prefix=set_namespace('cazart')&query=list()&user=${USER}&password=${PASSWORD}&save=dcsv"
curl -f -s -k "https://${host}:${port}/read_lines?id=${id}&n=0"
curl -f -s -k "https://${host}:${port}/release_session?id=${id}"
