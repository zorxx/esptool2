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

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>

#include "debug.h"
#include "ztool.h"
#include "ztool_elf.h"

#define IMAGE_PADDING   16
#define SECTION_PADDING 4
#define CHECKSUM_INIT   0xEF
#define BIN_MAGIC_FLASH 0xE9
#define SEPARATOR_LIST  " ,;"

#define ZBOOT_DEFAULT_BUILD_VERSION 0x00000001
#define ZBOOT_DEFAULT_BUILD_DESCRIPTION "zboot application"

typedef struct
{
    uint32_t addr;
    uint32_t size;
} Section_Header;

#define ZBOOT_MAGIC 0x279bfbf1

typedef struct
{
    uint32_t magic;
    uint32_t count;
    uint32_t entry;
    uint32_t version; 
    uint32_t date;
    uint32_t reserved[3];
    char     description[88];
} tzImageHeader;

typedef struct
{
    uint8_t  magic;
    uint8_t  count;
    uint8_t  flags1;
    uint8_t  flags2;
    uint32_t entry;
} tImageHeader;

static const char PADDING[IMAGE_PADDING] = {0};
uint8_t debug_level = 2;

// --------------------------------------------------------------------------------
// Helper Functions 

#define SECONDS_BETWEEN_1970_AND_2000 946684800L

uint32_t GetZbootTimestamp()
{
    uint32_t current = time(NULL);
    if(current < SECONDS_BETWEEN_1970_AND_2000)
       return 0;
    else
       return (current - SECONDS_BETWEEN_1970_AND_2000);
}

// Write an elf section (by name) to an existing file.
// Parameters:
//   headed - add a header to the output
//   zeroaddr - force zero entry point in header (default is the real entry point)
//   padded - output will be padded to multiple of SECTION_PADDING bytes
//   chksum - pointer to existing checksum to add this data to (zero if not needed)
// Produces error message on failure (so caller doesn't need to).
static bool WriteElfSection(MyElf_File *elf, FILE *fd, char* sectionNameList[], uint32_t sectionCount,
   bool addHeader, bool zeroAddress, uint32_t padto, void *chksum, uint32_t checksumSize)
{
   MyElf_Section **sections = NULL;
   bool success = true;
   uint8_t *data = NULL;
   uint32_t pad = 0;
   uint32_t totalSize = 0;
   uint32_t address = 0;
   uint32_t i;

   if(sectionCount <= 0)
      return true;  // Nothing to do?

   sections = (MyElf_Section **) malloc(sectionCount * sizeof(MyElf_Section *));
   if(NULL == sections)
   {
      ERROR("Failed to allocate memory for section list\n");
      return false;
   }

   // Get the information for all sections
   for(i = 0; success && i < sectionCount; ++i)
   {
      char *sectionName = sectionNameList[i];

      DEBUG("%s: Reading section '%s'\n", __func__, sectionName);
      sections[i] = GetElfSection(elf, sectionName);
      if(NULL == sections[i]) 
      {
         ERROR("Warning: Section '%s' not found in elf file.\n", sectionName);
      }
      else
      {
         uint32_t sectionSize = sections[i]->size;
         uint32_t offset = totalSize;

         if(0 == sectionSize)
         {
            DEBUG("Section '%s' is empty; skipping\n", sectionName);
            continue;
         }

         if(!zeroAddress && 0 == address)
            address = sections[i]->address;

         totalSize += sectionSize; 
         data = realloc(data, totalSize + padto); // Reserve enough space for max padding 
         if(NULL == data)
         {
             ERROR("%s: Failed to allocate buffer (%u bytes)\n", __func__, totalSize + padto);
             success = false;
         }
         else
         {
            uint8_t *buffer = GetElfSectionData(elf, sections[i], 0);
            if(NULL == buffer)
            {
               ERROR("%s: Failed to read data from ELF section '%s'\n", __func__, sectionName);
               success = false;
            }
            else
            {
               DEBUG("%s: Total size %u after %u section(s) (%s is %u bytes)\n",
                  __func__, totalSize, i+1, sectionName, sectionSize);
               memcpy(&data[offset], buffer, sectionSize);
               free(buffer);
            }
         }
      }
   }

   // Determine padding (if any)
   if(success && padto > 0)
   {
      pad = totalSize % padto;
      if(pad > 0)
      {
         pad = padto - pad;
         DEBUG("%s: Total length is %u bytes, padto %u bytes, padding is %u bytes\n",
            __func__, totalSize, padto, pad);
         memset(&data[totalSize], 0xa5, pad);  // pad bytes
         totalSize += pad;
      }
      else
      {
         DEBUG("%s: Total length is %u bytes, no padding needed (padto is %u)\n", __func__, totalSize, padto);
      }
   }

   // Calculate checksum of data
   if(success && NULL != chksum)
   {
      if(sizeof(uint32_t) == checksumSize)
      {
         for(uint32_t i = 0; i < totalSize; i += sizeof(uint32_t))
            *((uint32_t *) chksum) += *((uint32_t *) (data + i));
      }
      else if(sizeof(uint8_t) == checksumSize)
      {
         for(uint32_t i = 0; i < totalSize; ++i)
            *((uint8_t *) chksum) ^= data[i];
      }
      else
      {
         ERROR("%s; Invalid checksum size specified (%u)\n", __func__, checksumSize);
         success = false;
      }
   }

   if(success && addHeader)
   {
      Section_Header sechead;
      sechead.addr = address;
      sechead.size = totalSize;
      DEBUG("Adding section header: address %08x, size %08x\n", sechead.addr,
         sechead.size);
      if(fwrite(&sechead, 1, sizeof(sechead), fd) != sizeof(sechead))
      {
         ERROR("Failed to write header\n");
         success = false;
      }

      // 32-bit chechsums include the section header data
      if(sizeof(uint32_t) == checksumSize)
      {
         *((uint32_t *) chksum) += sechead.addr;
         *((uint32_t *) chksum) += sechead.size;
      }
   }
	
   if(success && totalSize > 0)
   {
      if(fwrite(data, 1, totalSize, fd) != totalSize) 
      {
         ERROR("Failed to write data (%u bytes)\n", totalSize); 
         success = false;
      }
   }

   if(NULL != sections)
      free(sections);
   if(NULL != data)
      free(data);

   return success; 
}


// --------------------------------------------------------------------------------
// Operations

// Load an elf file and export a section of it to a new file, without
// header, padding or checksum. For exporting the .irom0.text library.
// Produces error message on failure (so caller doesn't need to).
bool ExportElfSection(char *inFile, char *outFile, char *sectionName)
{
   MyElf_File *elf = NULL;
   FILE *fd = NULL;
   bool result = false;

   elf = LoadElf(inFile);
   if(NULL == elf)
   {
      ERROR("Error: Failed to open ELF file '%s'\n", inFile);
      return false;
   }

   fd = fopen(outFile, "wb");
   if(NULL == fd)
   { 
      ERROR("Error: Failed to open ELF file '%s'\n", inFile);
   }
   else
   {
      result = WriteElfSection(elf, fd, &sectionName, 1, false, false, 0, NULL, 0);
      fclose(fd);
   }        

   UnloadElf(elf);
   return result;
}

// Create the main binary firmware image, from specified elf sections.
// Can produce for standard standalone app (separate .irom0.text)
// or sdk bootloaded apps (integrated .irom0.text).
// Choice of type requires appropriately linked elf file.
// Produces error message on failure (so caller doesn't need to).
bool CreateHeaderFile(char *inFile, char *outFile, char *sections[], int numsec)
{
   MyElf_File *elf = NULL;
   FILE *fd = NULL;
   bool success = true;  // optimism

   elf = LoadElf(inFile);
   if(NULL == elf)
   {
      ERROR("Failed to open ELF file '%s'\n", inFile);
      return false;
   }
    
   fd = fopen(outFile, "wb");
   if(NULL == fd)
   {
      ERROR("Error: Failed to open output file '%s' for writing.\n", outFile);
      UnloadElf(elf);
      return false;
   }

   fprintf(fd, "#include <stdint.h>\n");
   fprintf(fd, "const uint32_t entry_addr = 0x%08x;\n", elf->header.e_entry);

   for (int i = 0; success && i < numsec; ++i)
   {
      char *sectionName = sections[i];
      MyElf_Section *sect = GetElfSection(elf, sectionName);
      if(NULL == sect)
      {
         ERROR("Failed to load section '%s'\n", sectionName);
         success = false;
      }
      else
      {
	 uint8_t *bindata = NULL;
         char name[31];
         size_t len;
         int j;
		
         strncpy(name, sect->name, 31);  // simple name fix name
         len = strlen(name);
         for(j = 0; j < len; j++)
            if (name[j] == '.') name[j] = '_';

         // add address, length and start the data block
         DEBUG("Adding section '%s', addr: 0x%08x, size: %d.\n", sectionName, sect->address, sect->size);
         fprintf(fd, "\nconst uint32_t %s_addr = 0x%08x;\nconst uint32_t %s_len = %d;\nconst uint8_t  %s_data[] = {",
            name, sect->address, name, sect->size, name);

         // get elf section binary data
         bindata = GetElfSectionData(elf, sect, 0);
         if(NULL == bindata)
         {
            ERROR("Failed to read data for section '%s'\n", sectionName);
            success = false;
         }
         else
         {
            for (j = 0; j < sect->size; j++)
            {
               if (j % 16 == 0)
                  fprintf(fd, "\r\n  0x%02x,", bindata[j]);
               else
                  fprintf(fd, " 0x%02x,", bindata[j]);
            }
            fprintf(fd, "\r\n};\r\n");
            free(bindata);
	 }
      }
   }
 
   fclose(fd);
   UnloadElf(elf);
   return success;	
}

bool CreateBinFile(char *inFile, char *outFile, uint8_t flashMode, uint8_t flashClock,
   uint8_t flashSize, char *romSectionList[], uint32_t romSectionCount,
   char *otherSectionList[], uint32_t otherSectionCount)
{
   MyElf_File *elf = NULL;
   FILE *fd = NULL;
   uint8_t chksum = CHECKSUM_INIT;
   bool success = true; // optimism
   uint32_t i;

   DEBUG("%s: Flash mode %u, size %u, clock %u, ROM sections %u, other sections %u\n", __func__,
      flashMode, flashSize, flashClock, romSectionCount, otherSectionCount);

   elf = LoadElf(inFile);
   if(NULL == elf)
   {
      ERROR("Failed to open ELF file '%s'\n", inFile);
      success = false;
   }
    
   if(success)
   {
      fd = fopen(outFile, "wb");
      if(NULL == fd)
      {
         ERROR("Failed to open output file '%s'\n", outFile);
         success = false;
      }
   }

   if(success)
   {
      tImageHeader imageHeader;
      imageHeader.magic = BIN_MAGIC_FLASH;
      imageHeader.count = otherSectionCount + ((romSectionCount > 0) ? 1 : 0); 
      imageHeader.flags1 = flashMode;
      imageHeader.flags2 = (flashSize << 4) | (flashClock & 0xf);
      imageHeader.entry = elf->header.e_entry;
      DEBUG("Image header: magic 0x%02x, section count %u, flags1 0x%02x, flags2 0x%02x, entry 0x%08x\n",
         imageHeader.magic, imageHeader.count, imageHeader.flags1, imageHeader.flags2,
         imageHeader.entry); 
      if(fwrite(&imageHeader, 1, sizeof(imageHeader), fd) != sizeof(imageHeader))
      {
         ERROR("Failed to write image header\n");
         success = false;
      }
   }
      
   // Write all of the ROM sections first, with just one header for all
   if(success && romSectionCount > 0 && NULL != romSectionList)
   {
      if(!WriteElfSection(elf, fd, romSectionList, romSectionCount, true, true, SECTION_PADDING,
         &chksum, sizeof(uint8_t)))
      {
         ERROR("Failed to write ROM section(s)\n");
         success = false;
      }
   }

   for(i = 0; success && i < otherSectionCount; ++i)
   {
      char *sectionName = otherSectionList[i];
      if(!WriteElfSection(elf, fd, &sectionName, 1, true, false, SECTION_PADDING, &chksum, sizeof(uint8_t)))
      {
         ERROR("Failed to write section '%s'\n", sectionName);
         success = false;
      }
   }
 
   if(success)
   {
      size_t len = ftell(fd) + sizeof(uint8_t);  // Total size, plus checksum
      uint32_t pad = len % IMAGE_PADDING;
      if (pad > 0)
      { 
         pad = IMAGE_PADDING - pad;
         DEBUG("%s: Padding image with %d byte(s).\n", __func__, pad);
         if(fwrite(PADDING, 1, pad, fd) != pad) 
         {
            ERROR("Error: Failed to write padding to image file.\n");
            success = false;
         }
      }
      else
      {
         DEBUG("%s: No image padding needed (size %lu, padto %u)\n", __func__, len, IMAGE_PADDING);
      }
   }

   if(success)
   {
      DEBUG("%s: Writing checksum 0x%02x\n", __func__, chksum);
      if(fwrite(&chksum, 1, sizeof(chksum), fd) != sizeof(chksum))
      {
         ERROR("Error: Failed to write checksum to image file.\n");
         success = false;
      }
   }

   if(NULL != fd)
      fclose(fd);
   if(NULL != elf)
      UnloadElf(elf);
	
   return success;
}

bool CreateZbootFile(char *inFile, char *outFile, uint32_t buildVersion, uint32_t buildDate, 
   char *buildDescription, char *romSectionList[], uint32_t romSectionCount,
   char *otherSectionList[], uint32_t otherSectionCount)
{
   MyElf_File *elf = NULL;
   FILE *fd = NULL;
   uint32_t chksum = 0; 
   bool success = true; // optimism
   uint32_t i;

   elf = LoadElf(inFile);
   if(NULL == elf)
   {
      ERROR("Failed to open ELF file '%s'\n", inFile);
      success = false;
   }
    
   if(success)
   {
      fd = fopen(outFile, "wb");
      if(NULL == fd)
      {
         ERROR("Failed to open output file '%s'\n", outFile);
         success = false;
      }
   }

   if(success)
   {
      tzImageHeader imageHeader;
      memset(&imageHeader, 0, sizeof(imageHeader));
      imageHeader.magic = ZBOOT_MAGIC; 
      imageHeader.count = otherSectionCount + ((romSectionCount > 0) ? 1 : 0); 
      imageHeader.entry = elf->header.e_entry;
      imageHeader.version = buildVersion;
      imageHeader.date = buildDate;
      if(NULL != buildDescription)
         strncpy(imageHeader.description, buildDescription, sizeof(imageHeader.description));
      else
         strcpy(imageHeader.description, ZBOOT_DEFAULT_BUILD_DESCRIPTION); 
      DEBUG("Image header: magic 0x%08x, count %u, entry 0x%08x, version 0x%08x, date 0x%08x, description '%s'\n",
         imageHeader.magic, imageHeader.count, imageHeader.entry, imageHeader.version, imageHeader.date,
         imageHeader.description);
      if(fwrite(&imageHeader, 1, sizeof(imageHeader), fd) != sizeof(imageHeader))
      {
         ERROR("Failed to write image header\n");
         success = false;
      }

      for(i = 0; i < sizeof(imageHeader); i += sizeof(uint32_t))
         chksum += *((uint32_t *)(((uint8_t *) &imageHeader) + i));
   }
   DEBUG("%s: Image header checksum = %08x\n", __func__, chksum);
      
   // Write all of the ROM sections first, with just one header for all
   if(success && romSectionCount > 0 && NULL != romSectionList)
   {
      if(!WriteElfSection(elf, fd, romSectionList, romSectionCount, true, true,
         SECTION_PADDING, &chksum, sizeof(chksum)))
      {
         ERROR("Failed to write ROM section(s)\n");
         success = false;
      }
   }

   for(i = 0; success && i < otherSectionCount; ++i)
   {
      char *sectionName = otherSectionList[i];
      if(!WriteElfSection(elf, fd, &sectionName, 1, true, false,
         SECTION_PADDING, &chksum, sizeof(chksum)))
      {
         ERROR("Failed to write section '%s'\n", sectionName);
         success = false;
      }
   }
 
   if(success)
   {
      DEBUG("%s: Writing checksum 0x%08x\n", __func__, chksum);
      if(fwrite(&chksum, 1, sizeof(chksum), fd) != sizeof(chksum))
      {
         ERROR("Error: Failed to write checksum to image file.\n");
         success = false;
      }
   }

   if(NULL != fd)
      fclose(fd);
   if(NULL != elf)
      UnloadElf(elf);
	
   return success;
}

// ----------------------------------------------------------------------------------------
// Main

static const char *programInfo =
   "ztool v2.0.0 - (c) 2018 Zorxx Software <zorxx@zorxx.com>\n" 
   "               (c) 2015 Richard A Burton <richardaburton@gmail.com>\n" 
   "This program is licensed under the GPL v3.\n"
   "See the file LICENSE for details.\n";

static const char *programUsage =
   "Usage:\n"
   "   [-h|-?]       Display program help\n"
   "   [-b|-l|-i|-z] Select output file type\n"
   "   -b            Create file suitable for ESP8266 boot ROM\n"
   "   -l            Create library file; a binary dump of one or more ELF sections\n"
   "   -i            Create a c/c++ header file from one or more ELF sections\n"
   "   -z            Create a file suitable for the zboot bootloader\n"
   "   -e <file>     Input (ELF) filename\n"
   "   -o <file>     Output filename\n"
   "   -s <sect.>    List of ELF sections to process. Allowed separators include\n"
   "                 space, comma, and semicolon\n" 
   "   -r <sect.>    List of ELF sections to include in zboot file. These sections\n"
   "                 are treated as ROM; not copied during the boot process.\n"
   "   -n <string>   Description of the application to include in zboot header\n"
   "   -v <hex>      Version (32-bit hext number) of application, included in zboot header\n"
   "   -c <size>     Flash capacity. Valid values are: 256k, 512K, 1M, 2M, 4M\n"
   "   -m <mode>     Flash more. Valid values are: dio, dout, qio, qout\n"
   "   -f <speed>    Flash frequency. Valid values are: 20, 26, 40, 80\n"
   "   -d <level>    Set the debug level (0 is least debug, 3 is most)\n"

   "Returns:\n"
   "   0 on success\n"
   "  -1 on failure\n";

typedef enum
{
   MODE_INVALID,
   MODE_LIBRARY,
   MODE_HEADER,
   MODE_BINARY,
   MODE_ZBOOT
} eOperation;

char **StringToList(char *string, char *separators, uint32_t *count)
{
   char **result = NULL;
   char *current = string;
   uint32_t c = 0;

   current = strtok(string, separators);
   while(NULL != current) 
   {
      result = (char **) realloc(result, (c+1) * sizeof(char *));
      if(NULL == result)
         return NULL;

      result[c++] = current; 
      current = strtok(NULL, separators); 
   }

   if(NULL != count)
      *count = c;
   return result;
}

int main(int argc, char *argv[])
{
   char *inFile = NULL;
   char *outFile = NULL;
   char **romSections = NULL;
   uint32_t romSectionCount = 0;
   char **otherSections = NULL;
   uint32_t otherSectionCount = 0;
   uint32_t buildVersion = ZBOOT_DEFAULT_BUILD_VERSION;
   char *buildDescription = NULL;
   eOperation operation = MODE_INVALID; 
   bool paramError = false;
   bool displayHelp = false;
   uint8_t flashMode = 0;
   uint8_t flashSize = 0;
   uint8_t flashClock = 0;
   int result = -1;
   int opt;

   while ((opt = getopt(argc, argv, "blihz?d:f:c:v:n:m:e:o:r:s:")) != -1)
   {
      switch (opt)
      {
         case 'h':   // Help
         case '?':
            displayHelp = true;
            break;
         case 'e':   // Input (ELF) file
            inFile = optarg;
            break;
         case 'o':   // Output file
            outFile = optarg;
            break;
         case 'b':   // binary file
            operation = MODE_BINARY; 
            break;
         case 'i':   // header (include) file
            operation = MODE_HEADER; 
            break;
         case 'l':   // library file
            operation = MODE_LIBRARY; 
            break;
         case 'z':   // zboot file
            operation = MODE_ZBOOT; 
            break;
         case 'd':   // debug level 
            debug_level = atoi(optarg); 
            break;
         case 'r':   // ROM section list
            romSections = StringToList(optarg, SEPARATOR_LIST, &romSectionCount);
            break;
         case 's':   // non-ROM section list
            otherSections = StringToList(optarg, SEPARATOR_LIST, &otherSectionCount);
            break;
         case 'v':   // build version 
            buildVersion = strtoul(optarg, NULL, 16);
            break;
         case 'n':   // build description 
            buildDescription = optarg; 
            break;
         case 'c':   // flash (capacity) size
            if(strcmp(optarg, "256") == 0
            || strcmp(optarg, "256K") == 0) 
               flashSize = 1;
            else if(strcmp(optarg, "512") == 0
            || strcmp(optarg, "512K") == 0)
               flashSize = 0;
            else if(strcmp(optarg, "1024") == 0
            || strcmp(optarg, "1M") == 0)
               flashSize = 2;
            else if(strcmp(optarg, "2048") == 0
            || strcmp(optarg, "2M") == 0)
               flashSize = 3;
            else if(strcmp(optarg, "4096") == 0
            || strcmp(optarg, "4M") == 0)
               flashSize = 4;
            else
            {
               error("Usupported flash size (%s)\n", optarg);
               paramError = true;
            }
            break;
         case 'm':   // flash mode
            if(strcmp(optarg, "qio") == 0)
               flashMode = 0;
            else if(strcmp(optarg, "qout") == 0)
               flashMode = 1;
            else if(strcmp(optarg, "dio") == 0)
               flashMode = 2;
            else if(strcmp(optarg, "dout") == 0)
               flashMode = 3;
            else
            {
               error("Usupported flash mode (%s)\n", optarg);
               paramError = true;
            }
            break;
         case 'f':   // flash frequency (speed) 
            if(strcmp(optarg, "20") == 0)
               flashClock = 2;
            else if(strcmp(optarg, "26.7") == 0
            || strcmp(optarg, "26") == 0)
               flashClock = 1;
            else if(strcmp(optarg, "40") == 0)
               flashClock = 0;
            else if(strcmp(optarg, "80") == 0)
               flashClock = 15;
            else
            {
               error("Usupported flash speed (%s)\n", optarg);
               paramError = true;
            }
            break;
         default:
            error("Usupported option (%c)\n", opt);
            paramError = true;
            break;
      }
   }

   PRINT("%s\n", programInfo);
   if(paramError)
   {
      ERROR("Parameter error\n");
      displayHelp = true;
   }

   if(displayHelp)
   {
      PRINT("%s\n", programUsage);
      return -1;
   }

   switch(operation)
   {
      case MODE_LIBRARY:
         if(NULL == inFile || NULL == outFile)
         {
            ERROR("Must specify input and output files\n");
         }
         else if (!ExportElfSection(inFile, outFile, ".irom0.text"))
         {
            ERROR("Failed to create library file\n");
         }
         else
         {
            PRINT("Successfully created library '%s'.\r\n", outFile);
            result = 0;
         }
         break;
      case MODE_HEADER:
         if(NULL == inFile || NULL == outFile)
         {
            ERROR("Must specify input and output files\n");
         }
         else if (!CreateHeaderFile(inFile, outFile, otherSections, otherSectionCount))
         {
            ERROR("Failed to create header file\n");
         }
         else
         {
            PRINT("Successfully created header file '%s'\r\n", outFile);
            result = 0;
         }
         break;
      case MODE_BINARY:
         if(NULL == inFile || NULL == outFile)
         {
            ERROR("Must specify input and output files\n");
         }
         else if (!CreateBinFile(inFile, outFile, flashMode, flashClock, flashSize,
            romSections, romSectionCount, otherSections, otherSectionCount))
         {
            ERROR("Failed to create binary file\n");
         }
         else
         {
            PRINT("Successfully created binary file '%s'\r\n", outFile);
            result = 0;
         }
         break;
      case MODE_ZBOOT:
         if(NULL == inFile || NULL == outFile)
         {
            ERROR("Must specify input and output files\n");
         }
         else if (!CreateZbootFile(inFile, outFile, buildVersion, GetZbootTimestamp(),
            buildDescription, romSections, romSectionCount, otherSections, otherSectionCount))
         {
            ERROR("Failed to create binary file\n");
         }
         else
         {
            PRINT("Successfully created binary file '%s'\r\n", outFile);
            result = 0;
         }
         break;
      default:
         ERROR("Unknown operation (%d)\n", operation);
         return -1;
   }

   return result;
}
