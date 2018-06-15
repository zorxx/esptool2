#
# Makefile for ztool
# https://github.com/zorxx/ztool
#

CFLAGS = -O2 -Wall
CC = gcc
LD = gcc

all: ztool

ztool.o: ztool.c ztool.h ztool_elf.h elf.h
	@echo "CC $<"
	@$(CC) $(CFLAGS) -c $< -o $@

ztool_elf.o: ztool_elf.c ztool.h ztool_elf.h elf.h
	@echo "CC $<"
	@$(CC) $(CFLAGS) -c $< -o $@

ztool: ztool.o ztool_elf.o
	@echo "LD $@"
	@$(LD) -o $@ $^

clean:
	@echo "RM *.o ztool ztool.exe"
	@rm -f *.o
	@rm -f ztool zool.exe
