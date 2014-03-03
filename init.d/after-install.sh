#!/bin/bash
# shim
# package post installation script
# This script is run by the package management system after the shim
# package is installed.

chmod 0755 /etc/init.d/shimsvc
mkdir -p /var/lib/shim
# Write out an example config file to /var/lib/shim/conf
cat >/var/lib/shim/conf << EOF
# Shim configuration file
# Uncomment and change any of the following values. Restart shim for
# your changes to take effect (default values are shown).

#auth=login
#ports=8080,8083s
#scidbport=1239
#tmp=/dev/shm/
#user=root
EOF
# Generate a certificate
openssl req -new -newkey rsa:4096 -days 3650 -nodes -x509 -subj "/C=US/ST=MA/L=Waltham/O=Paradigm4/CN=$(hostname)" -keyout /var/lib/shim/ssl_cert.pem 2>/dev/null >> /var/lib/shim/ssl_cert.pem
if test $? -ne 0; then
  echo "SSL certificate generation failed (openssl not found): TLS disabled."
  rm -f /var/lib/shim/ssl_cert.pem
fi
if test -n "$(which update-rc.d)"; then
# Ubuntu
  update-rc.d shimsvc defaults
elif test -n "$(which chkconfig)"; then
# RHEL
  chkconfig --add shimsvc && chkconfig shimsvc on
fi
/etc/init.d/shimsvc start
