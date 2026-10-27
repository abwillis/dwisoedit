# Makefile for isoedit (Dynamic Windows + bkisofs)
#
#   make BKDIR=/path/to/bkisofs
#
# DW_CFLAGS / DW_LIBS default to dwindows-config (installed with Dynamic
# Windows). Override them on the command line for other setups, e.g.
#   make DW_CFLAGS="-I/src/dwindows -D__OS2__" DW_LIBS="-L/src/dwindows -ldw"

CC      ?= cc
AR      ?= ar
BKDIR   ?= ../bkisofs
EXEEXT  ?=

DW_CFLAGS ?= $(shell dwindows-config --cflags)
DW_LIBS   ?= $(shell dwindows-config --libs)

# _FILE_OFFSET_BITS=64 must match the bkisofs build (VolInfo has off_t fields)
CFLAGS  += -std=gnu99 -Wall -O2 -D_FILE_OFFSET_BITS=64 -I$(BKDIR) $(DW_CFLAGS)
LDLIBS  += $(DW_LIBS) -lpthread

BKOBJS = bkRead7x.o bkAdd.o bkDelete.o bkExtract.o bkRead.o bkPath.o bkMangle.o \
         bkWrite.o bkWrite7x.o bkTime.o bkSort.o bkError.o bkGet.o bkSet.o \
         bkCache.o bkLink.o bkMisc.o bkIoWrappers.o
BKLIB  = $(BKDIR)/bk.a

.PHONY: all clean

all: isoedit$(EXEEXT)

isoedit$(EXEEXT): isoedit.c $(BKLIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ isoedit.c $(BKLIB) $(LDLIBS)

# Builds bk.a directly so this does not depend on the bkisofs Makefile
$(BKLIB): $(addprefix $(BKDIR)/,$(BKOBJS:.o=.c))
	cd $(BKDIR) && for f in $(BKOBJS:.o=); do \
	    $(CC) -std=gnu99 -D_FILE_OFFSET_BITS=64 -fPIC -c $$f.c -o $$f.o || exit 1; \
	done && $(AR) -cr bk.a $(BKOBJS)

clean:
	rm -f isoedit isoedit.exe
