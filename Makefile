ifeq ($(SCIDB),) 
  X := $(shell which scidb)
  ifneq ($(X),)
    X := $(shell dirname ${X})
    SCIDB := $(shell dirname ${X})
  endif
endif

CFLAGS=-fopenmp
INC=-I. -DPROJECT_ROOT="\"$(SCIDB)\"" -I"$(SCIDB)/include" -DSCIDB_CLIENT
LIBS=-ldl -lpthread -L"$(SCIDB)/lib" -lscidbclient -lboost_system

shim:
	$(CXX) $(INC) -fpic -g -c client.cpp -o client.o
	$(CC) -Wall $(CFLAGS) $(INC) -o shim shim.c mongoose.c client.o $(LIBS)

install: shim
	@if test ! -d "$(SCIDB)"; then echo  "Can't find scidb. Maybe try explicitly setting SCIDB variable, for example::\n\nmake SCIDB=/opt/scidb/13.2 install"; exit 1; fi 
	cp shim "$(SCIDB)/bin"
	mkdir -p /var/lib/shim
	cp -aR wwwroot /var/lib/shim/
	chmod -R 755 /var/lib/shim
	@if test -d /usr/local/share/man/man1;then cp man/shim.1 /usr/local/share/man/man1/;fi

uninstall: unservice
	@if test ! -d "$(SCIDB)"; then echo  "Can't find scidb. Maybe try explicitly setting SCIDB variable, for example:\n\nmake SCIDB=/opt/scidb/13.2 uninstall"; exit 1; fi 
	rm -f "$(SCIDB)/bin/shim"
	rm -rf /var/lib/shim
	rm -f /usr/local/share/man/man1/shim.1

service: install
	cp init.d/shimsvc /etc/init.d
	chmod 0755 /etc/init.d/shimsvc
	@if test -n "$$(which update-rc.d)"; then update-rc.d shimsvc defaults;fi
	@if test -n "$$(which chkconfig)"; then chkconfig --add shimsvc && chkconfig shimsvc on;fi
	/etc/init.d/shimsvc start

unservice:
	@if test -f /etc/init.d/shimsvc; then /etc/init.d/shimsvc stop; fi
	@if test -n "$$(which update-rc.d)"; then sudo update-rc.d -f shimsvc remove;fi
	@if test -n "$$(which chkconfig)"; then chkconfig --del shimsvc;fi
	rm -rf /etc/init.d/shimsvc

clean:
	rm -f *.o *.so shim
