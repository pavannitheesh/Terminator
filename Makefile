CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread
LDLIBS  = -lX11 -pthread
TARGET  = terminator

SRC = src/terminator.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
