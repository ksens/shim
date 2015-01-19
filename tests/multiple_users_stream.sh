#!/bin/bash
host=localhost
port=8088
td=$(mktemp -d)

mkdir -p $td/wwwroot
./shim -p $port -r $td/wwwroot  -f &
disown
sleep 1


# Test multiple users streaming their results

# SET THIS TO BE THE NUMBER OF REQUESTS TO ISSUE
N=20

# Call this with one argument indicating the number of doubles
# to download. For example:
# download 1000000

download(){
  s=`curl -s -k http://${host}:${port}/new_session`
  echo "session: $s"
  if test "${s}" != ""; then
    # The ugly url-encoded query string below is:
    # build(<x:double>[i=1:${1},10000,0],i)
    curl -s -k "http://${host}:${port}/execute_query?id=${s}&query=build(%3Cx%3Adouble%3E%5Bi%3D1%3A${1}%2C10000%2C0%5D%2Ci)&save=(double)&stream=1"
    echo
    curl -s -k "http://${host}:${port}/read_bytes?id=${s}&n=0" | dd of=/dev/null
    echo "Done with session: ${s}"
  else
    echo "Resource not available"
  fi
}

j=0
echo Round 1
while test $j -lt $N;do
  download 10000000 &
  j=$(($j + 1))
done
sleep 5
j=0
echo "Round 2 (many expected to have resource unavailable errors)"
while test $j -lt $N;do
  download 10000000 &
  j=$(($j + 1))
done

wait


rm -rf $td
killall -9 shim
