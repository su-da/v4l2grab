MAINTARGET := v4l2grab
SOURCE := v4l2grab.c decoder_mjpeg.c
CFLAGS += -Wall -D_REENTRANT
LDFLAGS += -lv4l2 -lSDL2 -lSDL2_image
OBJS := ${SOURCE:.c=.o}

all: $(MAINTARGET)

$(MAINTARGET): $(OBJS)
	$(LINK.o) $^ $(OUTPUT_OPTION)

clean:
	-$(RM) $(MAINTARGET) $(OBJS)

.PHONY: clean
