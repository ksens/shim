Shim is a super-basic SciDB client that exposes limited SciDB functionality
through a simple HTTP API. It's based on the mongoose web server.

The program must run on the system that a SciDB coordinator runs on. It does
not need to run as the same user that SciDB runs under, but the users do need
to be in the same group.

Note: libscidbclient.so must be in shim's library path. This may entail setting
LD_LIBRARY_PATH=/opt/scidb/<whatever>/lib  before running shim.  You don't have
to worry about that if you install and run shim as a service.

Note: Shim queries are limited to at most 1,000,000 characters.

See the wwwroot/api.html document for the API documentation, or compile and
start shim running and point a browser to http://localhost:8080/api.html.
You can also preview the api.html page directly from github at:

[Paradigm4/shim/master/wwwroot/api.html](http://htmlpreview.github.com/?https://raw.github.com/Paradigm4/shim/master/wwwroot/api.html)


The wwwroot directory also includes an example simple javascript client.

##Installation from binary packages
This is the fastest/easiest way to install this service.
The author (Bryan Lewis) provides a few pre-built binary packages for SciDB 13.3 and 13.6 on Ubuntu 12.04 here:

* [http://illposed.net/shim_13.3_amd64.deb](http://illposed.net/shim_13.3_amd64.deb)
* [http://illposed.net/shim_13.6_amd64.deb](http://illposed.net/shim_13.6_amd64.deb)

```
# Install with:
sudo gdebi shim_13.6_amd64.deb

# Uninstall with:
apt-get remove shim
```

and for SciDB 13.3 and SciDB13.6 on RHEL/Centos 6.3 here:

* [http://illposed.net/shim-13.3-1.x86_64.rpm](http://illposed.net/shim-13.3-1.x86_64.rpm)
* [http://illposed.net/shim-13.6-1.x86_64.rpm](http://illposed.net/shim-13.6-1.x86_64.rpm)

```
# Install with:
rpm -i shim-13.6-1.x86_64.rpm
# shim depends on libgomp. If installation fails, install libgomp and try again:
yum install libgomp

# Uninstall with:
yum remove shim
```
I will continue to make binary packages available when new versions of SciDB are released.


##Compile and Install from Source
Note that because shim is a SciDB client it needs the boost, log4cpp and log4cxx development libraries installed to compile. We illustrate installation of Ubuntu build dependencies below:
```
sudo apt-get install liblog4cpp5-dev liblog4cxx10-dev libboost-dev libboost-system-dev rubygems
gem install fpm
```
Once the build dependencies are install, build shim with:
```
make
sudo make install

# Or, if SCIDB is not in the PATH, can set a Make variable SCIDB that points
# to the SCIDB home directory, for example for version 13.3:

make SCIDB=/opt/scidb/13.3
sudo make SCIDB=/opt/scidb/13.3 install

```
### Optionally install as a service
You can install shim as a system service so that it just runs all the time with:
```
sudo make SCIDB=/opt/scidb/13.3 service
```
If you install shim as a service and want to change its default options, for example the default HTTP port or port to talk to SciDB on, you'll need to edit the /etc/init.d/shimsvc file. See the discussion of command line parameters below.
### Optionally build deb or rpm packages
You can build the service version of shim into packages for Ubuntu 12.04 or RHEL/CentOS 6 with
```
make deb-pkg
make rpm-pkg
```
respectively. Building packages requires that certain extra packaging programs are available,
including rpmbuild for RHEL/CentOS and the Ruby-based fpm packaging utility on all systems.

## Usage
```
shim [-h] [-f] [-p <http port>] [-r <document root>] [-s <scidb port>]
```
where, -f means run in the foreground (defaults to background), -h means help.

If you installed the service version, then you can control when shim is running with the usual mechanism, for example:
```
/etc/init.d/shimsvc stop
/etc/init.d/shimsvc start
```

## Uninstall
We explicitly define our SCIDB home directory for Make in the example below:
```
sudo make SCIDB=/opt/scidb/13.3 uninstall
```


## Log files
Shim prints messages to the system log. The syslog file location varies, but can usually be found in /var/log/syslog or /var/log/messages.
