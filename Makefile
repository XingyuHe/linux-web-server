CC = gcc
CFLAGS = -g -Wall -Werror
LDFLAGS = -g -pthread

TARGETS = multi-threaded-server
OBJS = multi-server.o

$(TARGETS):
$(OBJS):

PHONY += clean
clean:
	rm -rf $(TARGETS) a.out *.o

.PHONY: $(PHONY)
