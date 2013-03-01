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


##Compilation
(assumes scidb is in the PATH):
```
make
sudo make install
```
Note that because shim is a SciDB client it needs the boost, log4cpp and log4cxx development libraries installed to compile.

##Usage
```
shim [-h] [-f] [-p <http port>] [-r <document root>] [-s <scidb port>]
```
where, -f means run in the foreground (defaults to background), -h means help.
