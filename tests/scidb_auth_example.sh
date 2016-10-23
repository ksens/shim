#!/bin/bash

# Example illustrating SciDB authentication over TLS using the default user
# name 'root' and password 'Paradigm4'. We don't run this as an automated test,
# this is just an example that shows how to do this.

PASSWORD=Paradigm4
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
curl -f -s -k "https://${host}:${port}/execute_query?id=${id}&query=create_namespace('cazart')&user=${USER}&password=${PASSWORD}&release=1" > /dev/null
echo "create_namespace('cazart')"

# Add an array to the cazart namespace
id=$(curl -s -k "https://${host}:${port}/new_session")
curl -f -s -k "https://${host}:${port}/execute_query?id=${id}&prefix=set_namespace('cazart')&query=store(list(),yikes)&user=${USER}&password=${PASSWORD}&release=1" > /dev/null

# context (list the contents of the 'cazart' namespace)
id=$(curl -s -k "https://${host}:${port}/new_session")
curl -f -s -k "https://${host}:${port}/execute_query?id=${id}&prefix=set_namespace('cazart')&query=list()&user=${USER}&password=${PASSWORD}&save=dcsv" > /dev/null
curl -f -s -k "https://${host}:${port}/read_lines?id=${id}&n=0"
curl -f -s -k "https://${host}:${port}/release_session?id=${id}"

#It's good to clean up after yourself
id=$(curl -s -k "https://${host}:${port}/new_session")
curl -f -s -k "https://${host}:${port}/execute_query?id=${id}&query=remove(cazart.yikes)&user=${USER}&password=${PASSWORD}" > /dev/null
curl -f -s -k "https://${host}:${port}/release_session?id=${id}"

id=$(curl -s -k "https://${host}:${port}/new_session")
curl -f -s -k "https://${host}:${port}/execute_query?id=${id}&query=drop_namespace('cazart')&user=${USER}&password=${PASSWORD}" > /dev/null
curl -f -s -k "https://${host}:${port}/release_session?id=${id}"

#Test the negative case:
PASSWORD=wrong
echo 
echo "You should see an authentication error here:"
id=$(curl -s -k "https://${host}:${port}/new_session")
curl -s -k "https://${host}:${port}/execute_query?id=${id}&query=list()&user=${USER}&password=${PASSWORD}"
echo
