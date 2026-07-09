CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
TARGETS = name_server storage_server client

.PHONY: all clean

all: $(TARGETS)

name_server: name_server.c common.c common.h
	$(CC) $(CFLAGS) -o name_server name_server.c common.c

storage_server: storage_server.c common.c common.h
	$(CC) $(CFLAGS) -o storage_server storage_server.c common.c

client: client.c common.c common.h
	$(CC) $(CFLAGS) -o client client.c common.c

clean:
	rm -f $(TARGETS) *.o
	rm -rf .storage/

test: all
	@echo "Starting servers and running test..."
	@echo "Make sure to run servers manually in separate terminals"
