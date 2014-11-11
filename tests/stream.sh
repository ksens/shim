host=localhost
port=8080
td=$(mktemp -d)

mkdir -p $td/wwwroot
./shim -p $port -r $td/wwwroot  -f &
sleep 1

s=`curl -s -k http://${host}:${port}/new_session`
echo "session: $s"
curl -s -k "http://${host}:${port}/execute_query?id=${s}&query=list('functions')&save=dcsv&stream=1"
echo
curl -s -k "http://${host}:${port}/read_bytes?id=${s}&n=0"


killall shim
rm -rf $td
