CC = gcc
CFLAGS = -Wall -Wextra -g -std=c99

mysh: mysh.c
	$(CC) $(CFLAGS) -o mysh mysh.c

clean:
	rm -f mysh *.o p3.tar
