s=`curl -s -k http://localhost:8080/new_session`
echo "session: $s"
curl -s -k "http://localhost:8080/execute_query?id=${s}&query=list('functions')&save=dcsv&stream=1"
echo
curl -s -k "http://localhost:8080/read_bytes?id=${s}&n=0"
