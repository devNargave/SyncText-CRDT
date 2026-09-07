CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -pthread -lrt
TARGET = editor

# All source files
SOURCES = editor.c file_utils.c user_registry.c message_queue.c crdt_merge.c display.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET) clear_users

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS)

clear_users: clear_users.o user_registry.o
	$(CC) $(CFLAGS) -o clear_users clear_users.o user_registry.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) clear_users $(OBJECTS) clear_users.o
	rm -f *_doc.txt
	rm -f /dev/mqueue/queue_*

.PHONY: all clean

