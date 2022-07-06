CFLAGS = -ggdb3 -Wall -Wno-psabi
# no-psabi - disable warnings for changes in aa64 ABI
# no-tree-sra - tree-sra causes unit-test to segfault on x86-64, not on aa64
# note that no-unswitch-loops also gets rid of the segfault
CXXFLAGS = -std=c++17 -ggdb3 -Wall -Wno-psabi -fno-tree-sra
SOFLAGS = -shared -fPIC

liblsvd.so: lsvd_rbd.cc extent.h journal2.h objects.h
	g++ -std=c++17 lsvd_rbd.cc -o liblsvd.so $(OPT) $(CXXFLAGS) $(SOFLAGS) -lstdc++fs -lpthread -lrados -lrt -laio

lsvd_rbd.o: lsvd_rbd.cc extent.h journal2.h smartiov.h objects.h
	g++ -c -std=c++17 lsvd_rbd.cc $(OPT) $(CXXFLAGS) 

bdus: bdus.o lsvd_rbd.o extent.h journal2.h
	g++ lsvd_rbd.o bdus.o -o bdus $(CFLAGS) $(CXXFLAGS) -lbdus -lpthread -lstdc++fs -lrados

mkdisk: mkdisk.cc objects.h
	g++ mkdisk.cc -o mkdisk $(CXXFLAGS) -luuid -lstdc++fs

clean:
	rm -f liblsvd.so bdus mkdisk

unit-test: unit-test.cc extent.h
	g++ $(OPT) $(CXXFLAGS) -o unit-test unit-test.cc -lstdc++fs

unit-test-O3: unit-test.cc extent.h
	g++ $(CXXFLAGS) -O3 -o $@ unit-test.cc -lstdc++fs

check_lsvd_syms: lsvd.py liblsvd.so
	fgrep 'lsvd_lib.' lsvd.py | sed -e 's/.*lsvd_lib.//' -e 's/(.*//' > /tmp/_syms
	nm liblsvd.so | grep U | if fgrep -f /tmp/_syms; then false; else true; fi
	rm -f /tmp/_syms
