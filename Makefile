CC = gcc
CFLAGS = -Wall -Wextra -g -std=c99

mysh: mysh.c parse.c execute.c job.h execute.h
	$(CC) $(CFLAGS) -o mysh mysh.c parse.c execute.c

clean:
	rm -f mysh *.o p3.tar
