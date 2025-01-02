BASEDIR = deps/reef/
include $(BASEDIR)Make.env
include Make.version

APP = sucker

SOURCES = $(wildcard *.c)
OBJECTS = $(SOURCES:.c=.o)

INCS = $(INCBASE) -I ./deps/minimp3/ -I ./deps/dr_libs/
LIBS = $(LIBBASE)

INCS += -I/usr/include/alsa
LIBS += -lm -lasound

all: $(APP) version.h

DEPEND = .depend
$(DEPEND): $(SOURCES)
	@$(CC) $(CFLAGS) -MM $^ $(INCS) > $@

version.h: Make.version
	$(call generate_version, version.h)

include $(DEPEND)
%.o: %.c
	$(CC) $(CFLAGS) $(INCS) -o $@ -c $<

sucker: 0main.o rpi.o bee.o net.o client.o binary.o timer.o packet.o
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

test: test.o
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

clean:
	rm -f $(OBJECTS) $(APP)
