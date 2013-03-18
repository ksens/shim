#!/bin/bash
# shim
# package post installation script
# This script is run by the package management system after the shim
# package is installed.

chmod 0755 /etc/init.d/shimsvc
if test -n "$(which update-rc.d)"; then
# Ubuntu
  update-rc.d shimsvc defaults
elif test -n "$(which chkconfig)"; then
# RHEL
  chkconfig --add shimsvc && chkconfig shimsvc on
fi
/etc/init.d/shimsvc start
