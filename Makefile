MAINTARGET := v4l2grab
SOURCE := v4l2grab.c
LDFLAGS += -lv4l2
OBJS := ${SOURCE:.c=.o}

all: $(MAINTARGET)

$(MAINTARGET): $(OBJS)
	$(LINK.o) $^ $(OUTPUT_OPTION)

clean:
	-$(RM) $(MAINTARGET) $(OBJS)

.PHONY: clean
