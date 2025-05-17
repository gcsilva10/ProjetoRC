CC = gcc
CFLAGS = -Wall -Wextra -std=c11

# Source files
CLIENT_LIB = client.c
SERVER_SRC = server.c
TEST_SRC = test_client.c

# Headers
HEADERS = powerudp.h

# Output executables
SERVER_BIN = servidor
TEST_BIN = test_client

.PHONY: all clean

all: $(SERVER_BIN) $(TEST_BIN)

$(SERVER_BIN): $(SERVER_SRC) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRC)

$(TEST_BIN): $(TEST_SRC) $(CLIENT_LIB) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(CLIENT_LIB) -lpthread

clean:
	rm -f $(SERVER_BIN) $(TEST_BIN)
