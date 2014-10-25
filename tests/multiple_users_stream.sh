#!/bin/bash

# Test multiple users streaming their results

# SET THIS TO BE THE NUMBER OF REQUESTS TO ISSUE
N=50

# Call this with one argument indicating the number of doubles
# to download. For example:
# download 1000000

download(){
  s=`curl -s -k http://localhost:8080/new_session`
  echo "session: $s"
  if test "${s}" != ""; then
    # The ugly url-encoded query string below is:
    # build(<v:double>[i=1:${1},10000,0],i)
    curl -s -k "http://localhost:8080/execute_query?id=${s}&query=build(%3Cx%3Adouble%3E%5Bi%3D1%3A${1}%2C10000%2C0%5D%2Ci)&save=(double)&stream=1"
    echo
    curl -s -k "http://localhost:8080/read_bytes?id=${s}&n=0" | dd of=/dev/null
    echo "Done with session: ${s}"
  else
    echo "Resource not available"
  fi
  curl -s --digest --user homer:elmo "http://localhost:8080/release_session?id=${s}"
}

j=0
while test $j -lt $N;do
  download 10000000 &
  j=$(($j + 1))
done
wait
