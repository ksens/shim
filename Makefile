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

install:
	@if test ! -d "$(SCIDB)"; then echo  "Can't find scidb. Maybe try explicitly setting SCIDB:\n\nmake SCIDB=/opt/scidb/13.2 install"; exit 1; fi 
	cp shim "$(SCIDB)/bin"
	mkdir -p /var/lib/shim
	cp -aR wwwroot /var/lib/shim/
	chmod -R 755 /var/lib/shim

uninstall:
	@if test ! -d "$(SCIDB)"; then echo  "Can't find scidb. Maybe try explicitly setting SCIDB:\n\nmake SCIDB=/opt/scidb/13.2 install"; exit 1; fi 
	rm -f "$(SCIDB)/bin/shim"
	rm -rf /var/lib/shim

clean:
	rm -f *.o *.so shim
