Shim is a super-basic SciDB client that exposes certain SciDB functionality
through a simple HTTP API. It's based on the mongoose http service.

The program must run on the system that a SciDB coordinator runs on, It does
not need to run as the same user that SciDB runs under, but the users do need
to be in the same group.

Note: libscidbclient.so must be in shim's library path. This may entail
setting LD_LIBRARY_PATH=/opt/scidb/<whatever>/lib  before running shim.

See the wwwroot/api.html document for the API documentation, or compile and
start shim running and point a browser to http://localhost:8080/api.html.
You can also preview the api.html page directly from github at:

[Paradigm4/shim/master/wwwroot/api.html](http://htmlpreview.github.com/?https://raw.github.com/Paradigm4/shim/master/wwwroot/api.html)


The wwwroot directory also includes an example simple javascript client.


##Compile and Install
Note that because shim is a SciDB client it needs the boost, log4cpp and log4cxx development libraries installed to compile.
```
make
sudo make install

# Or, if SCIDB is not in the PATH, can set a Make variable SCIDB that points
# to the SCIDB home directory, for example for version 13.1:

make SCIDB=/opt/scidb/13.1
sudo make SCIDB=/opt/scidb/13.1 install

```
### Optionally install as a service
You can install shim as a system service so that it just runs all the time with:
```
sudo make SCIDB=/opt/scidb/13.1 service
```
If you install shim as a service and want to change its default options, for example the default HTTP port or port to talk to SciDB on, you'll need to edit the /etc/init.d/shimsvc file. See the discussion of command line parameters below.

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
sudo make SCIDB=/opt/scidb/13.1 uninstall
```



## Log files
Shim prints messages to the system log. The syslog file location varies, but can usually be found in /var/log/syslog.
