CC = gcc
CFLAGS = -Wall -Wextra -g -std=c99

mysh: mysh.c parse.c job.h
	$(CC) $(CFLAGS) -o mysh mysh.c parse.c

clean:
	rm -f mysh *.o p3.tar
