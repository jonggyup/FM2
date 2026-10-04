HEMEMDIR = $(CURDIR)/../src/

CC = gcc
#CFLAGS = -g -Wall -O3
CFLAGS = -g3 -Wall -O2
INCLUDES = -I../linux/usr/include
LIBS = -lm -lpthread

default: mem-access

mem-access: mem_access.c
	$(CC) $(CFLGAS) $(INCLUDES) -o mem-access mem_access.c $(LIBS)

clean:
	$(RM) *.o mem-access