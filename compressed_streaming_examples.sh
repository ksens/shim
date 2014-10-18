# Basic compressed stream example using default compression
s=`wget -O - -q http://localhost:8080/new_session`
echo "session: $s"
wget -O - -q "http://localhost:8080/execute_query?id=${s}&query=list('functions')&save=dcsv&stream=2"
echo
wget -O - -q "http://localhost:8080/read_bytes?id=${s}&n=0" >/tmp/z.gz
# now we can gunzip this, for example: gunzip /tmp/z.gz && cat /tmp/z


# Compressed stream example specifying level:
s=`wget -O - -q http://localhost:8080/new_session`
echo "session: $s"
wget -O - -q "http://localhost:8080/execute_query?id=${s}&query=list('functions')&save=dcsv&stream=2&compression=9"
echo
wget -O - -q "http://localhost:8080/read_bytes?id=${s}&n=0" >/tmp/z.gz
# now we can gunzip this, for example: gunzip /tmp/z.gz && cat /tmp/z
