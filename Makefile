MAINTARGET := v4l2grab
SOURCE := v4l2grab.c
CFLAGS += -Wall
EXLDFLAGS += -lv4l2
OBJS := ${SOURCE:.c=.o}
CC = $(CROSS_COMPILE)gcc
ifdef CROSS_COMPILE
    CFLAGS += --sysroot=$(SYSROOT)
    EXLDFLAGS += --sysroot=$(SYSROOT)
endif

all: $(MAINTARGET)

$(MAINTARGET): $(OBJS)
	$(LINK.o) $^ $(EXLDFLAGS) $(OUTPUT_OPTION)

clean:
	-$(RM) $(MAINTARGET) $(OBJS)

.PHONY: clean
