Shim is a super-basic SciDB client that exposes certain SciDB functionality
through a simple HTTP API. It's based on the mongoose http service.

The program must run on the system that a SciDB coordinator runs on, It does
not need to run as the same user that SciDB runs under, but the users do need
to be in the same group.

See the wwwroot/api.html document for the API documentation, or compile and
start shim running and point a browser to http://localhost:8080/api.html

The wwwroot directory also includes an example simple javascript client.


Compilation (assumes scidb is in the PATH):
make

(Note that because shim is a SciDB client it needs the boost, log4cpp and
log4cxx development libraries installed to compile.)

Usage:
shim [-h] [-d] [-p <http port>] [-r <document root>] [-s <scidb port>]

where, -d means run in a background process, -h means help.
