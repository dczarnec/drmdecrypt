/* drmdecrypt -- DRM decrypting tool for Samsung TVs
 *
 * Copyright (C) 2014 - Bernhard Froehlich <decke@bluelife.at>
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the GPL v2 license.  See the LICENSE file for details.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
#ifdef _MSC_VER
#include "w32\w32.h"
#else
#include <libgen.h>
#include <unistd.h>
#include <cpuid.h>
#endif


#include "aes.h"
#include "trace.h"
#include "buffer.h"

/* Helper macros */
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

/* Version Information */
#ifndef REVISION
#define REVISION  ""
#endif
#define VERSION	  "1.0"

block_state state;
int enable_aesni = 0;


/*
 * Check for AES-NI CPU support
 */
int Check_CPU_support_AES()
{
#if defined(__INTEL_COMPILER)
   int CPUInfo[4] = {-1};
   __cpuid(CPUInfo, 1);
   return (CPUInfo[2] & 0x2000000);
#else
   unsigned int a=1,b,c,d;
   __cpuid(1, a,b,c,d);
   return (c & 0x2000000);
#endif
}


char *filename(char *path, char *newsuffix)
{
   char *end = path + strlen(path);

   while(*end != '.' && *end != '/')
      --end;

   if(newsuffix != NULL)
      strcpy(++end, newsuffix);
   else
      *end = '\0';

   return path;
}

int readdrmkey(char *mdbfile)
{
   unsigned char drmkey[0x10];
   char tmpbuf[64];
   unsigned int j;
   FILE *mdbfp;

   memset(tmpbuf, '\0', sizeof(tmpbuf));
   memset(&state, 0, sizeof(block_state));
   state.rounds = 10;

   if((mdbfp = fopen(mdbfile, "rb")))
   {
      fseek(mdbfp, 8, SEEK_SET);
      for (j = 0; j < 0x10; j++){
         if(fread(&drmkey[(j&0xc)+(3-(j&3))], sizeof(unsigned char), 1, mdbfp) != 1){
            trace(TRC_ERROR, "short read while reading DRM key");
            return 1;
         }
      }
      fclose(mdbfp);

      for (j = 0; j < sizeof(drmkey); j++)
         sprintf(tmpbuf+strlen(tmpbuf), "%02X ", drmkey[j]);

      trace(TRC_INFO, "drm key successfully read from %s", basename(mdbfile));
      trace(TRC_INFO, "KEY: %s", tmpbuf);

      if(enable_aesni)
         block_init_aesni(&state, drmkey, BLOCK_SIZE);
      else
         block_init_aes(&state, drmkey, BLOCK_SIZE);

      return 0;
   }
   else
      trace(TRC_ERROR, "mdb file %s not found", basename(mdbfile));

   return 1;
}

int genoutfilename(char *outfile, char *inffile)
{
   FILE *inffp;
   unsigned char inf[0x200];
   char tmpname[PATH_MAX];
   int i;

   if((inffp = fopen(inffile, "rb")))
   {
      fseek(inffp, 0, SEEK_SET);
      if(fread(inf, sizeof(unsigned char), 0x200, inffp) != 0x200){
         trace(TRC_ERROR, "short read while reading inf file");
         return 1;
      }
      fclose(inffp);

      /* build base path */
      strcpy(tmpname, basename(inffile));
      filename(tmpname, NULL);
      strcat(tmpname, "-");
      
      /* http://code.google.com/p/samy-pvr-manager/wiki/InfFileStructure */

      /* copy channel name and program title */
      for(i=1; i < 0x200; i += 2)
      {
         if (inf[i])
         {
            if((inf[i] >= 'A' && inf[i] <= 'z') || (inf[i] >= '0' && inf[i] <= '9'))
               strncat(tmpname, (char*)&inf[i], 1);
            else
               strcat(tmpname, "_");
         }
         if (i == 0xFF) {
            strcat(tmpname, "_-_");
         }
      }

      strcat(tmpname, ".ts");

      strcat(outfile, tmpname);
   }
   else
      return 1;

   return 0;
}


int decrypt_aes128cbc(unsigned char *pin, int len, unsigned char *pout)
{
   int i;

   if(len % BLOCK_SIZE != 0)
   {
      trace(TRC_ERROR, "Decrypt length needs to be a multiple of BLOCK_SIZE");
      return 1;
   }

   for(i=0; i < len; i+=BLOCK_SIZE)
   {
      if(enable_aesni)
         block_decrypt_aesni(&state, pin + i, pout + i);
      else
         block_decrypt_aes(&state, pin + i, pout + i);
   }

   return 0;
}


/*
 * Decode a MPEG packet
 *
 * Transport Stream Header:
 * ========================
 *
 * Name                    | bits | byte msk | Description
 * ------------------------+------+----------+-----------------------------------------------
 * sync byte               | 8    | 0xff     | Bit pattern from bit 7 to 0 as 0x47
 * Transp. Error Indicator | 1    | 0x80     | Set when a demodulator cannot correct errors from FEC data
 * Payload Unit start ind. | 1    | 0x40     | Boolean flag with a value of true means the start of PES
 *                         |      |          | data or PSI otherwise zero only.
 * Transport Priority      | 1    | 0x20     | Boolean flag with a value of true means the current packet
 *                         |      |          | has a higher priority than other packets with the same PID.
 * PID                     | 13   | 0x1fff   | Packet identifier
 * Scrambling control      | 2    | 0xc0     | 00 = not scrambled
 *                         |      |          | 01 = Reserved for future use (DVB-CSA only)
 *                         |      |          | 10 = Scrambled with even key (DVB-CSA only)
 *                         |      |          | 11 = Scrambled with odd key (DVB-CSA only)
 * Adaptation field exist  | 1    | 0x20     | Boolean flag
 * Contains payload        | 1    | 0x10     | Boolean flag
 * Continuity counter      | 4    | 0x0f     | Sequence number of payload packets (0x00 to 0x0F)
 *                         |      |          | Incremented only when a playload is present
 *
 * Adaptation Field:
 * ========================
 *
 * Name                    | bits | byte msk | Description
 * ------------------------+------+----------+-----------------------------------------------
 * Adaptation Field Length | 8    | 0xff     | Number of bytes immediately following this byte
 * Discontinuity indicator | 1    | 0x80     | Set to 1 if current TS packet is in a discontinuity state
 * Random Access indicator | 1    | 0x40     | Set to 1 if PES packet starts a video/audio sequence
 * Elementary stream prio  | 1    | 0x20     | 1 = higher priority
 * PCR flag                | 1    | 0x10     | Set to 1 if adaptation field contains a PCR field
 * OPCR flag               | 1    | 0x08     | Set to 1 if adaptation field contains a OPCR field
 * Splicing point flag     | 1    | 0x04     | Set to 1 if adaptation field contains a splice countdown field
 * Transport private data  | 1    | 0x02     | Set to 1 if adaptation field contains private data bytes
 * Adapt. field extension  | 1    | 0x01     | Set to 1 if adaptation field contains extension
 * Below fields optional   |      |          | Depends on flags
 * PCR                     | 33+6+9 |        | Program clock reference
 * OPCR                    | 33+6+9 |        | Original Program clock reference
 * Splice countdown        | 8    | 0xff     | Indicates how many TS packets from this one a splicing point
 *                         |      |          | occurs (may be negative)
 * Stuffing bytes          | 0+   |          |
 *
 *
 * See: http://en.wikipedia.org/wiki/MPEG_transport_stream
 */
int decode_packet(unsigned char *data)
{
   unsigned char tmp[PACKETSIZE];
   int offset;

   if(data[0] != 0x47)
   {
      trace(TRC_ERROR, "Not a valid MPEG packet!");
      return 1;
   }

   memcpy(tmp, data, PACKETSIZE);
   
   trace(TRC_DEBUG, "-------------------");
   trace(TRC_DEBUG, "Trans. Error Indicator: 0x%x", data[2] & 0x80);
   trace(TRC_DEBUG, "Payload Unit start Ind: 0x%x", data[2] & 0x40);
   trace(TRC_DEBUG, "Transport Priority    : 0x%x", data[2] & 0x20);
   trace(TRC_DEBUG, "Scrambling control    : 0x%x", data[3] & 0xC0);
   trace(TRC_DEBUG, "Adaptation field exist: 0x%x", data[3] & 0x20);
   trace(TRC_DEBUG, "Contains payload      : 0x%x", data[3] & 0x10);
   trace(TRC_DEBUG, "Continuity counter    : 0x%x", data[3] & 0x0f);

   /* only process scrambled content */
   if(((data[3] & 0xC0) != 0xC0) && ((data[3] & 0xC0) != 0x80))
     return 1;

   if(data[3] & 0x20)
	   trace(TRC_DEBUG, "Adaptation Field length: 0x%x", data[4]+1);

   offset=4;

   /* skip adaption field */
   if(data[3] & 0x20)
      offset += (data[4]+1);

   /* remove scrambling bits */
   tmp[3] &= 0x3f;

   /* decrypt only full blocks (they seem to avoid padding) */
   decrypt_aes128cbc(data + offset, ((PACKETSIZE - offset)/BLOCK_SIZE)*BLOCK_SIZE, tmp + offset);

   memcpy(data, tmp, sizeof(tmp));

   return 0;
}

int decryptsrf(char *srffile, char *outdir)
{
   char mdbfile[PATH_MAX];
   char inffile[PATH_MAX];
   char outfile[PATH_MAX];
   struct packetbuffer pb;
   int retries, sync_find = 0;
   unsigned long filesize = 0;
   unsigned long i;

   memset(&pb, '\0', sizeof(pb));
   memset(inffile, '\0', sizeof(inffile));
   memset(mdbfile, '\0', sizeof(mdbfile));
   memset(outfile, '\0', sizeof(outfile));

   strcpy(inffile, srffile);
   filename(inffile, "inf");

   strcpy(mdbfile, srffile);
   filename(mdbfile, "mdb");

   /* read drm key from .mdb file */
   if(readdrmkey(mdbfile) != 0)
      return 1;

   /* generate outfile name based on title from .inf file */
   strcpy(outfile, outdir);
   if(genoutfilename(outfile, inffile) != 0)
   {
      strcat(outfile, srffile);
      filename(outfile, "ts");
   }

   trace(TRC_INFO, "Writing to %s", outfile);

   pbinit(&pb);

#ifdef _MSC_VER
   int wmode = _S_IWRITE;
   int binaryflag =  _O_BINARY;
#else
   mode_t wmode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
   int binaryflag = 0;
#endif
   pb.fdwrite = open(outfile, O_WRONLY | O_CREAT | O_TRUNC | binaryflag, wmode);
   if(pb.fdwrite == -1)
   {
      trace(TRC_ERROR, "Cannot open %s for writing", outfile);
      return 1;
   }

   pb.fdread = open(srffile, O_RDONLY | binaryflag);
   if(pb.fdread == -1)
   {
      trace(TRC_ERROR, "Cannot open %s for reading", srffile);
      return 1;
   }
	

   /* calculate filesize */
   filesize = lseek(pb.fdread, 0, SEEK_END);
   lseek(pb.fdread, 0, SEEK_SET);

   trace(TRC_INFO, "Filesize %ld", filesize);

resync:

   /* try to sync */
   sync_find = 0;
   retries = 10;

   while(sync_find == 0 && retries-- > 0)
   {
      pbread(&pb);

      /* search packets starting with 0x47 */
      for(i=0; i < (BUFFERSIZE-PACKETSIZE-PACKETSIZE); i++)
      {
         if (*(pb.workp+i) == 0x47 && *(pb.workp+i+PACKETSIZE) == 0x47 && *(pb.workp+i+PACKETSIZE+PACKETSIZE) == 0x47)
         {
            sync_find = 1;
            pb.workp += i;

            trace(TRC_INFO, "synced at offset %ld", pb.workp-pb.startp);

            break;
         }
      }
   }

   if (sync_find)
   {
      while(pb.end == 0)
      {
         pbread(&pb);

         while(pb.workp+PACKETSIZE <= pb.endp)
         {
            if (*(pb.workp) == 0x47)
            {
               decode_packet((unsigned char *)pb.workp);
               pb.workp += PACKETSIZE;
            }
            else
            {
               pbwrite(&pb);
               goto resync;
            }
         }

         pbwrite(&pb);
      }
   }

   pbwrite(&pb);

   close(pb.fdwrite);
   close(pb.fdread);
   pbfree(&pb);

   return 0;
}

void usage(void)
{
   fprintf(stderr, "Usage: drmdecrypt [-dqvx][-o outdir] infile.srf ...\n");
   fprintf(stderr, "Options:\n");
   fprintf(stderr, "   -d         Show debugging output\n");
   fprintf(stderr, "   -o outdir  Output directory\n");
   fprintf(stderr, "   -q         Be quiet. Only error output.\n");
   fprintf(stderr, "   -v         Version information\n");
   fprintf(stderr, "   -x         Disable AES-NI support\n");
   fprintf(stderr, "\n");
}

int main(int argc, char *argv[])
{
   char outdir[PATH_MAX];
   int ch;

   memset(outdir, '\0', sizeof(outdir));

   enable_aesni = Check_CPU_support_AES();

   while ((ch = getopt(argc, argv, "do:qvx")) != -1)
   {
      switch (ch)
      {
         case 'd':
            if(tracelevel > TRC_DEBUG)
               tracelevel--;
            break;
         case 'o':
            strcpy(outdir, optarg);
            break;
         case 'q':
            if(tracelevel < TRC_ERROR)
               tracelevel++;
            break;
         case 'v':
            fprintf(stderr, "drmdecrypt %s (%s)\n\n", VERSION, STR(REVISION));
            fprintf(stderr, "Source: http://github.com/decke/drmdecrypt\n");
            fprintf(stderr, "License: GNU General Public License\n");
            exit(EXIT_SUCCESS);
         case 'x':
            enable_aesni = 0;
            break;
         default:
            usage();
            exit(EXIT_FAILURE);
      }
   }

   if(argc == optind)
   {
      usage();
      exit(EXIT_FAILURE);
   }

   /* set and verify outdir */
   if(strlen(outdir) < 1)
      strcpy(outdir, dirname(argv[optind]));

   if(outdir[strlen(outdir)-1] != '/')
      strcat(outdir, "/");

   trace(TRC_INFO, "AES-NI CPU support %s", enable_aesni ? "enabled" : "disabled");

   do
   {
      if(decryptsrf(argv[optind], outdir) != 0)
         break;
   }
   while(++optind < argc);

   return 0;
}

