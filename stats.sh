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

# Host name
  n=$(hostname)
# Percent free memory
  mem=$(echo "scale=2;100*($free + $cached + $buffers - $shmem)/$total" | bc)
# one minute load average
  load=$(cat /proc/loadavg | cut -d ' ' -f 1)
# Percent free SciDB storage devices (might be more than one, | delimited)
  store=$(for x in `ps aux | grep scidb | grep "\-s" | grep "\-p" | sed -e "s/.*-s //" | cut -d ' ' -f 1 | sort | uniq`; do df -h `dirname $x` | sed 1d; done | sort | uniq | tr -s ' ' | cut -d ' ' -f 1,5  |sed -e "s/%//" | sort | uniq | tr '\n' '|')
  msg="${n},${mem},${load},${store}"
  wget -O - -q "http://${1}/measurement?data=${msg}" >/dev/null 2>&1
  sleep 4
done
