#!/bin/bash
# Collect basic node statistics and report them to shim

while true;
do
  M=$(cat /proc/meminfo)
  total=$(echo "${M}" | grep "^MemTotal" | cut -d ':' -f 2 | sed -e "s/^ *//" | sed -e "s/ .*//")
  free=$(echo "${M}" | grep "^MemFree" | cut -d ':' -f 2 | sed -e "s/^ *//" | sed -e "s/ .*//")
  cached=$(echo "${M}" | grep "^Cached" | cut -d ':' -f 2 | sed -e "s/^ *//" | sed -e "s/ .*//")
  buffers=$(echo "${M}" | grep "^Buffers" | cut -d ':' -f 2 | sed -e "s/^ *//" | sed -e "s/ .*//")
  shmem=$(echo "${M}" | grep "^Shmem" | cut -d ':' -f 2 | sed -e "s/^ *//" | sed -e "s/ .*//")

  n=$(hostname)   # Host name
# Percent free memory
  mem=$(echo "scale=2;100*($free + $cached + $buffers - $shmem)/$total" | bc)
# one minute load average
  load=$(cat /proc/loadavg | cut -d ' ' -f 1)
  msg="${n},${mem},${load}"
  wget -O - -q "http://${1}/measurement?data=${msg}" >/dev/null 2>&1
  sleep 2
done
