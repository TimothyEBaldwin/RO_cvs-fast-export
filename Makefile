GCC_WARNINGS1=-Wall -Wpointer-arith -Wstrict-prototypes
GCC_WARNINGS2=-Wmissing-prototypes -Wmissing-declarations
GCC_WARNINGS3=-fno-strict-aliasing
GCC_WARNINGS4=-Wno-unused-function -Wno-unused-label
GCC_WARNINGS=$(GCC_WARNINGS1) $(GCC_WARNINGS2) $(GCC_WARNINGS3)  $(GCC_WARNINGS4)
GITPATH=/usr/local/src/git
CFLAGS=-O2 -g $(GCC_WARNINGS) -I$(GITPATH) -DSHA1_HEADER='<openssl/sha.h>'
LIBS=-L$(GITPATH) -lgit $(GITPATH)/xdiff/lib.a -lssl -lcrypto -lz -lpthread
YFLAGS=-d -l
LFLAGS=-l

OBJS=gram.o lex.o parsecvs.o cvsutil.o revdir.o \
	revlist.o atom.o revcvs.o generate.o git.o gitutil.o \
	nodehash.o tags.o tree.o authormap.o

parsecvs: $(OBJS)
	cc $(CFLAGS) -o $@ $(OBJS) $(LIBS)

$(OBJS): cvs.h
lex.o: y.tab.h

lex.o: lex.c

y.tab.h: gram.c

clean:
	rm -f $(OBJS) y.tab.h gram.c lex.c parsecvs
install:
	cp parsecvs edit-change-log ${HOME}/bin
