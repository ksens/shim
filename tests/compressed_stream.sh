# Basic compressed stream example using default compression
s=`curl -s -k http://localhost:8080/new_session`
echo "session: $s"
curl -s -k "http://localhost:8080/execute_query?id=${s}&query=list('functions')&save=dcsv&stream=2"
echo
curl -s -k "http://localhost:8080/read_bytes?id=${s}&n=0" >/tmp/z.gz
# Decompress
gunzip -c /tmp/z.gz
rm /tmp/z.gz


# Compressed stream example specifying level:
s=`curl -s -k http://localhost:8080/new_session`
echo "session: $s"
curl -s -k "http://localhost:8080/execute_query?id=${s}&query=list('functions')&save=dcsv&stream=2&compression=9"
echo
curl -s -k "http://localhost:8080/read_bytes?id=${s}&n=0" >/tmp/z.gz
# Decompress
gunzip -c /tmp/z.gz
rm /tmp/z.gz
