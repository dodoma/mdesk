BASEDIR = deps/reef/
include $(BASEDIR)Make.env
include Make.version

APP = sucker

SOURCES = $(wildcard *.c)
OBJECTS = $(SOURCES:.c=.o)

INCS = $(INCBASE)
LIBS = $(LIBBASE)

LIBS += -lm

all: $(APP) version.h

DEPEND = .depend
$(DEPEND): $(SOURCES)
	@$(CC) $(CFLAGS) -MM $^ $(INCS) > $@

version.h: Make.version
	$(call generate_version, version.h)

include $(DEPEND)
%.o: %.c
	$(CC) $(CFLAGS) $(INCS) -o $@ -c $<

sucker: 0main.o rpi.o net.o client.o timer.o packet.o
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

clean:
	rm -f $(OBJECTS) $(APP)
