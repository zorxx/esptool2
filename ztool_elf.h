/**********************************************************************************
*
*    Copyright 2018 Zorxx Software <zorxx@zorxx.com> 
*    Copyright (c) 2015 Richard A Burton <richardaburton@gmail.com>
*
*    This file is part of ztool, based on esptool2.
*
*    ztool is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    ztool is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with ztool.  If not, see <http://www.gnu.org/licenses/>.
*
**********************************************************************************/

#ifndef ZTOOL_ELF_H
#define ZTOOL_ELF_H

#include <stdio.h>
#include <stdint.h>

#include "elf.h"

typedef struct 
{
   Elf32_Off    offset;
   Elf32_Addr   address;
   Elf32_Word   size;
   char        *name;
} MyElf_Section;

typedef struct 
{
   FILE           *fd;
   Elf32_Ehdr      header;
   char           *strings;
   MyElf_Section  *sections;
} MyElf_File;

MyElf_File* LoadElf(char *infile);
void UnloadElf(MyElf_File *e_object);
MyElf_Section* GetElfSection(MyElf_File *e_object, char *name);
unsigned char* GetElfSectionData(MyElf_File *e_object, MyElf_Section *section, uint8_t pad);

#endif /* ZTOOL_ELF_H */
