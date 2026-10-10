CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDLIBS  = -lX11
TARGET  = x11_helloworld

SRC = x11_helloworld.c
all: run
	$(TARGET): $(SRC)
		$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)


run: $(TARGET)
	./$(TARGET)
	rm -f $(TARGET)

clean:
	rm -f $(TARGET)
