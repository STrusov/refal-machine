TARGET = refal
INSTALLDIR = /bin

SOURCES_ROOT = $(PROJECT_ROOT)src/
HEADERS := $(notdir $(wildcard $(SOURCES_ROOT)*.h))
SOURCES := main.c interpreter.c library.c message_print.c translator.c

CFLAGS  := -std=c18 -Wall

ifeq ($(BUILD_MODE),debug)
	CFLAGS  := -DDEBUG -g $(CFLAGS) $(DEFINES)
else ifeq ($(BUILD_MODE),run)
	CFLAGS  := -O2 $(CFLAGS) $(DEFINES)
else ifeq ($(BUILD_MODE),)
	CFLAGS  := -DNDEBUG -O2 -flto $(CFLAGS) $(DEFINES)
	LDFLAGS := -flto $(LDFLAGS)
else
	$(error Build mode $(BUILD_MODE) not supported by this Makefile)
endif

OBJECTS = $(notdir $(SOURCES:.c=.o))
PROJECT_ROOT = $(dir $(lastword $(MAKEFILE_LIST)))

.PHONY: all clean install uninstall test

all:	$(TARGET)

$(TARGET):	$(OBJECTS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(OBJECTS):%.o:	$(SOURCES_ROOT)%.c $(addprefix $(SOURCES_ROOT),$(HEADERS))
	$(CC) -c $(CFLAGS) -o $@ $<

clean:
	$(RM) $(TARGET) $(OBJECTS)

test:	$(TARGET)
	ls  $(PROJECT_ROOT)tests/*.ref | while read filename ; do \
	  echo $${filename}; \
	  ./$(TARGET) +n "$${filename}" | diff - "$${filename}.эталон"; \
	done

install:	$(TARGET)
	install --strip $(TARGET) $(INSTALLDIR)

uninstall:
	rm --force $(INSTALLDIR)/$(TARGET)
