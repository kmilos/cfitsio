/*  This file, buffers.c, contains the core set of FITSIO routines         */
/*  that use or manage the internal set of IO buffers.                     */

/*  The FITSIO software was written by William Pence at the High Energy    */
/*  Astrophysic Science Archive Research Center (HEASARC) at the NASA      */
/*  Goddard Space Flight Center.  Users shall not, without prior written   */
/*  permission of the U.S. Government,  establish a claim to statutory     */
/*  copyright.  The Government and others acting on its behalf, shall have */
/*  a royalty-free, non-exclusive, irrevocable,  worldwide license for     */
/*  Government purposes to publish, distribute, translate, copy, exhibit,  */
/*  and perform such material.                                             */

#include <string.h>
#include <stdlib.h>
#include "fitsio2.h"

char iobuffer[NIOBUF][IOBUFLEN];      /* initialize to zero by default  */
FITSfile *bufptr[NIOBUF];             /* initialize to zero by default  */
long bufrecnum[NIOBUF];               /* initialize to zero by default  */
int dirty[NIOBUF], ageindex[NIOBUF];  /* ages get initialized in ffwhbf */

/*--------------------------------------------------------------------------*/
int ffmbyt(fitsfile *fptr,    /* I - FITS file pointer                */
           long bytepos,      /* I - byte position in file to move to */
           int err_mode,      /* I - 1=ignore error, 0 = return error */
           int *status)       /* IO - error status                    */
{
/*
  Move to the input byte location in the file.  When writing to a file, a move
  may sometimes be made to a position beyond the current EOF.  The err_mode
  parameter determines whether such conditions should be returned as an error
  or simply ignored.
*/
    long record;

    if (*status > 0)
       return(*status);

    if (bytepos < 0)
        return(*status = NEG_FILE_POS);

    if (fptr->HDUposition != (fptr->Fptr)->curhdu)
        ffmahd(fptr, (fptr->HDUposition) + 1, NULL, status);

    record = bytepos / IOBUFLEN;  /* zero-indexed record number */

    /* if this is not the current record, then load it */
    if (record != bufrecnum[(fptr->Fptr)->curbuf]) 
        ffldrc(fptr, record, err_mode, status);

    if (*status <= 0)
        (fptr->Fptr)->bytepos = bytepos;  /* save new file position */

    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffpbyt(fitsfile *fptr,   /* I - FITS file pointer                    */
           long nbytes,      /* I - number of bytes to write             */
           void *buffer,     /* I - buffer containing the bytes to write */
           int *status)      /* IO - error status                        */
/*
  put (write) the buffer of bytes to the output FITS file, starting at
  the current file position.  Write large blocks of data directly to disk;
  write smaller segments to intermediate IO buffers to improve efficiency.
*/
{
    int ii, nbuff;
    long filepos, recstart, recend;
    long ntodo, bufpos, nspace, nwrite;
    char *cptr;

    if (*status > 0)
       return(*status);

    if (fptr->HDUposition != (fptr->Fptr)->curhdu)
        ffmahd(fptr, (fptr->HDUposition) + 1, NULL, status);

    cptr = (char *)buffer;
    ntodo =  nbytes;

    if (nbytes >= MINDIRECT)
    {
      /* write large blocks of data directly to disk instead of via buffers */
      /* first, fill up the current IO buffer before flushing it to disk */

      nbuff = (fptr->Fptr)->curbuf;      /* current IO buffer number */
      filepos = (fptr->Fptr)->bytepos;   /* save the write starting position */
      recstart = bufrecnum[nbuff];                 /* starting record */
      recend = (filepos + nbytes - 1) / IOBUFLEN;  /* ending record   */

      /* bufpos is the starting position within the IO buffer */
      bufpos = filepos - (recstart * IOBUFLEN);
      nspace = IOBUFLEN - bufpos;   /* amount of space left in the buffer */

      if (nspace)
      { /* fill up the IO buffer */
        memcpy(iobuffer[nbuff] + bufpos, cptr, nspace);
        ntodo -= nspace;           /* decrement remaining number of bytes */
        cptr += nspace;            /* increment user buffer pointer */
        filepos += nspace;         /* increment file position pointer */
        dirty[nbuff] = TRUE;       /* mark record as having been modified */
      }

      for (ii = 0; ii < NIOBUF; ii++) /* flush any affected buffers to disk */
      {
        if (bufptr[ii] == fptr->Fptr && bufrecnum[ii] >= recstart
            && bufrecnum[ii] <= recend )
        {
          if (dirty[ii])        /* flush modified buffer to disk */
             ffbfwt(ii, status);

          bufptr[ii] = NULL;  /* disassociate buffer from the file */
        }
      }

      /* move to the correct write position */
      if ((fptr->Fptr)->io_pos != filepos)
         ffseek(fptr->Fptr, filepos);

      nwrite = ((ntodo - 1) / IOBUFLEN) * IOBUFLEN; /* don't write last buff */

      ffwrite(fptr->Fptr, nwrite, cptr, status); /* write the data */
      ntodo -= nwrite;                /* decrement remaining number of bytes */
      cptr += nwrite;                  /* increment user buffer pointer */
      (fptr->Fptr)->io_pos = filepos + nwrite; /* update the file position */

      if ((fptr->Fptr)->io_pos >= (fptr->Fptr)->filesize) /* at the EOF? */
      {
        (fptr->Fptr)->filesize = (fptr->Fptr)->io_pos; /* increment file size */

        /* initialize the current buffer with the correct fill value */
        if ((fptr->Fptr)->hdutype == ASCII_TBL)
          memset(iobuffer[nbuff], 32, IOBUFLEN);  /* blank fill */
        else
          memset(iobuffer[nbuff],  0, IOBUFLEN);  /* zero fill */
      }
      else
      {
        /* read next record */
        ffread(fptr->Fptr, IOBUFLEN, iobuffer[nbuff], status);
        (fptr->Fptr)->io_pos += IOBUFLEN; 
      }

      /* copy remaining bytes from user buffer into current IO buffer */
      memcpy(iobuffer[nbuff], cptr, ntodo);
      dirty[nbuff] = TRUE;       /* mark record as having been modified */
      bufrecnum[nbuff] = recend; /* record number */
      bufptr[nbuff] = fptr->Fptr;  /* file pointer associated with IO buffer */

      (fptr->Fptr)->logfilesize = maxvalue((fptr->Fptr)->logfilesize, 
                                            (recend + 1) * IOBUFLEN);
      (fptr->Fptr)->bytepos = filepos + nwrite + ntodo;
    }
    else
    {
      /* bufpos is the starting position in IO buffer */
      bufpos = (fptr->Fptr)->bytepos - (bufrecnum[(fptr->Fptr)->curbuf] *
               IOBUFLEN);
      nspace = IOBUFLEN - bufpos;   /* amount of space left in the buffer */

      while (ntodo)
      {
        nwrite = minvalue(ntodo, nspace);

        /* copy bytes from user's buffer to the IO buffer */
        memcpy(iobuffer[(fptr->Fptr)->curbuf] + bufpos, cptr, nwrite);
        ntodo -= nwrite;            /* decrement remaining number of bytes */
        cptr += nwrite;
        (fptr->Fptr)->bytepos += nwrite;  /* increment file position pointer */
        dirty[(fptr->Fptr)->curbuf] = TRUE; /* mark record as modified */

        if (ntodo)                  /* load next record into a buffer */
        {
          ffldrc(fptr, (fptr->Fptr)->bytepos / IOBUFLEN, IGNORE_EOF, status);
          bufpos = 0;
          nspace = IOBUFLEN;
        }
      }
    }
    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffpbytoff(fitsfile *fptr, /* I - FITS file pointer                   */
           long gsize,        /* I - size of each group of bytes         */
           long ngroups,      /* I - number of groups to write           */
           long offset,       /* I - size of gap between groups          */
           void *buffer,      /* I - buffer to be written                */
           int *status)       /* IO - error status                       */
/*
  put (write) the buffer of bytes to the output FITS file, with an offset
  between each group of bytes.  This function combines ffmbyt and ffpbyt
  for increased efficiency.
*/
{
    int bcurrent;
    long ii, bufpos, nspace, nwrite, record;
    char *cptr, *ioptr;

    if (*status > 0)
       return(*status);

    if (fptr->HDUposition != (fptr->Fptr)->curhdu)
        ffmahd(fptr, (fptr->HDUposition) + 1, NULL, status);

    cptr = (char *)buffer;
    bcurrent = (fptr->Fptr)->curbuf;     /* number of the current IO buffer */
    record = bufrecnum[bcurrent];  /* zero-indexed record number */
    bufpos = (fptr->Fptr)->bytepos - (record * IOBUFLEN); /* starting pos. */
    nspace = IOBUFLEN - bufpos;  /* amount of space left in buffer */
    ioptr = iobuffer[bcurrent] + bufpos;  

    for (ii = 1; ii < ngroups; ii++)  /* write all but the last group */
    {
      /* copy bytes from user's buffer to the IO buffer */
      nwrite = minvalue(gsize, nspace);
      memcpy(ioptr, cptr, nwrite);
      cptr += nwrite;          /* increment buffer pointer */

      if (nwrite < gsize)        /* entire group did not fit */
      {
        dirty[bcurrent] = TRUE;  /* mark record as having been modified */
        record++;
        ffldrc(fptr, record, IGNORE_EOF, status);  /* load next record */
        bcurrent = (fptr->Fptr)->curbuf;
        ioptr   = iobuffer[bcurrent];

        nwrite  = gsize - nwrite;
        memcpy(ioptr, cptr, nwrite);
        cptr   += nwrite;            /* increment buffer pointer */
        ioptr  += (offset + nwrite); /* increment IO buffer pointer */
        nspace = IOBUFLEN - offset - nwrite;  /* amount of space left */
      }
      else
      {
        ioptr  += (offset + nwrite);  /* increment IO bufer pointer */
        nspace -= (offset + nwrite);
      }

      if (nspace <= 0) /* beyond current record? */
      {
        dirty[bcurrent] = TRUE;
        record += ((IOBUFLEN - nspace) / IOBUFLEN); /* new record number */
        ffldrc(fptr, record, IGNORE_EOF, status);
        bcurrent = (fptr->Fptr)->curbuf;

        bufpos = (-nspace) % IOBUFLEN; /* starting buffer pos */
        nspace = IOBUFLEN - bufpos;
        ioptr = iobuffer[bcurrent] + bufpos;  
      }
    }
      
    /* now write the last group */
    nwrite = minvalue(gsize, nspace);
    memcpy(ioptr, cptr, nwrite);
    cptr += nwrite;          /* increment buffer pointer */

    if (nwrite < gsize)        /* entire group did not fit */
    {
      dirty[bcurrent] = TRUE;  /* mark record as having been modified */
      record++;
      ffldrc(fptr, record, IGNORE_EOF, status);  /* load next record */
      bcurrent = (fptr->Fptr)->curbuf;
      ioptr   = iobuffer[bcurrent];

      nwrite  = gsize - nwrite;
      memcpy(ioptr, cptr, nwrite);
    }

    dirty[bcurrent] = TRUE;    /* mark record as having been modified */
    (fptr->Fptr)->bytepos = (fptr->Fptr)->bytepos + (ngroups * gsize)
                                  + (ngroups - 1) * offset;
    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffgbyt(fitsfile *fptr,    /* I - FITS file pointer             */
           long nbytes,       /* I - number of bytes to read       */
           void *buffer,      /* O - buffer to read into           */
           int *status)       /* IO - error status                 */
/*
  get (read) the requested number of bytes from the file, starting at
  the current file position.  Read large blocks of data directly from disk;
  read smaller segments via intermediate IO buffers to improve efficiency.
*/
{
    int ii;
    long filepos, recstart, recend, ntodo, bufpos, nspace, nread;
    char *cptr;

    if (*status > 0)
       return(*status);

    if (fptr->HDUposition != (fptr->Fptr)->curhdu)
        ffmahd(fptr, (fptr->HDUposition) + 1, NULL, status);
    cptr = (char *)buffer;

    if (nbytes >= MINDIRECT)
    {
      /* read large blocks of data directly from disk instead of via buffers */
      filepos = (fptr->Fptr)->bytepos; /* save the read starting position */

      recstart = bufrecnum[(fptr->Fptr)->curbuf];          /* starting record */
      recend = (filepos + nbytes - 1) / IOBUFLEN;  /* ending record   */

      for (ii = 0; ii < NIOBUF; ii++) /* flush any affected buffers to disk */
      {
        if (dirty[ii] && bufptr[ii] == fptr->Fptr && 
            bufrecnum[ii] >= recstart && bufrecnum[ii] <= recend)
            {
              ffbfwt(ii, status);    /* flush modified buffer to disk */
            }
      }

       /* move to the correct read position */
      if ((fptr->Fptr)->io_pos != filepos)
         ffseek(fptr->Fptr, filepos);

      ffread(fptr->Fptr, nbytes, cptr, status); /* read the data */
      (fptr->Fptr)->io_pos = filepos + nbytes; /* update the file position */
    }
    else
    {
      /* read small chucks of data using the IO buffers for efficiency */

      /* bufpos is the starting position in IO buffer */
      bufpos = (fptr->Fptr)->bytepos - (bufrecnum[(fptr->Fptr)->curbuf] *
                IOBUFLEN);
      nspace = IOBUFLEN - bufpos;   /* amount of space left in the buffer */

      ntodo =  nbytes;
      while (ntodo)
      {
        nread  = minvalue(ntodo, nspace);

        /* copy bytes from IO buffer to user's buffer */
        memcpy(cptr, iobuffer[(fptr->Fptr)->curbuf] + bufpos, nread);
        ntodo -= nread;            /* decrement remaining number of bytes */
        cptr  += nread;
        (fptr->Fptr)->bytepos += nread;    /* increment file position pointer */

        if (ntodo)                  /* load next record into a buffer */
        {
          ffldrc(fptr, (fptr->Fptr)->bytepos / IOBUFLEN, REPORT_EOF, status);
          bufpos = 0;
          nspace = IOBUFLEN;
        }
      }
    }

    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffgbytoff(fitsfile *fptr, /* I - FITS file pointer                   */
           long gsize,        /* I - size of each group of bytes         */
           long ngroups,      /* I - number of groups to read            */
           long offset,       /* I - size of gap between groups          */
           void *buffer,      /* I - buffer to be filled                 */
           int *status)       /* IO - error status                       */
/*
  get (read) the requested number of bytes from the file, starting at
  the current file position.  This function combines ffmbyt and ffgbyt
  for increased efficiency.
*/
{
    int bcurrent;
    long ii, bufpos, nspace, nread, record;
    char *cptr, *ioptr;

    if (*status > 0)
       return(*status);

    if (fptr->HDUposition != (fptr->Fptr)->curhdu)
        ffmahd(fptr, (fptr->HDUposition) + 1, NULL, status);

    cptr = (char *)buffer;
    bcurrent = (fptr->Fptr)->curbuf;     /* number of the current IO buffer */
    record = bufrecnum[bcurrent];  /* zero-indexed record number */
    bufpos = (fptr->Fptr)->bytepos - (record * IOBUFLEN); /* starting  pos. */
    nspace = IOBUFLEN - bufpos;  /* amount of space left in buffer */
    ioptr = iobuffer[bcurrent] + bufpos;  

    for (ii = 1; ii < ngroups; ii++)  /* read all but the last group */
    {
      /* copy bytes from IO buffer to the user's buffer */
      nread = minvalue(gsize, nspace);
      memcpy(cptr, ioptr, nread);
      cptr += nread;          /* increment buffer pointer */

      if (nread < gsize)        /* entire group did not fit */
      {
        record++;
        ffldrc(fptr, record, REPORT_EOF, status);  /* load next record */
        bcurrent = (fptr->Fptr)->curbuf;
        ioptr   = iobuffer[bcurrent];

        nread  = gsize - nread;
        memcpy(cptr, ioptr, nread);
        cptr   += nread;            /* increment buffer pointer */
        ioptr  += (offset + nread); /* increment IO buffer pointer */
        nspace = IOBUFLEN - offset - nread;  /* amount of space left */
      }
      else
      {
        ioptr  += (offset + nread);  /* increment IO bufer pointer */
        nspace -= (offset + nread);
      }

      if (nspace <= 0) /* beyond current record? */
      {
        record += ((IOBUFLEN - nspace) / IOBUFLEN); /* new record number */
        ffldrc(fptr, record, REPORT_EOF, status);
        bcurrent = (fptr->Fptr)->curbuf;

        bufpos = (-nspace) % IOBUFLEN; /* starting buffer pos */
        nspace = IOBUFLEN - bufpos;
        ioptr = iobuffer[bcurrent] + bufpos;  
      }
    }

    /* now read the last group */
    nread = minvalue(gsize, nspace);
    memcpy(cptr, ioptr, nread);
    cptr += nread;          /* increment buffer pointer */

    if (nread < gsize)        /* entire group did not fit */
    {
      record++;
      ffldrc(fptr, record, REPORT_EOF, status);  /* load next record */
      bcurrent = (fptr->Fptr)->curbuf;
      ioptr   = iobuffer[bcurrent];

      nread  = gsize - nread;
      memcpy(cptr, ioptr, nread);
    }

    (fptr->Fptr)->bytepos = (fptr->Fptr)->bytepos + (ngroups * gsize)
                                  + (ngroups - 1) * offset;
    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffldrc(fitsfile *fptr,        /* I - FITS file pointer             */
           long record,           /* I - record number to be loaded    */
           int err_mode,          /* I - 1=ignore EOF, 0 = return EOF error */
           int *status)           /* IO - error status                 */
{
/*
  low-level routine to load a specified record from a file into
  a physical buffer, if it is not already loaded.  Reset all
  pointers to make this the new current record for that file.
  Update ages of all the physical buffers.
*/
    int ibuff, nbuff;
    long rstart;

    /* check if record is already loaded in one of the buffers */
    /* search from youngest to oldest buffer for efficiency */

    if (fptr->HDUposition != (fptr->Fptr)->curhdu)
        ffmahd(fptr, (fptr->HDUposition) + 1, NULL, status);

    for (ibuff = NIOBUF - 1; ibuff >= 0; ibuff--)
    {
      nbuff = ageindex[ibuff];
      if (bufptr[nbuff] == fptr->Fptr && record == bufrecnum[nbuff])
         goto updatebuf;  /* use 'goto' for efficiency */
    }

    /* record is not already loaded */
    rstart = record * IOBUFLEN;

    if ( !err_mode && (rstart >= (fptr->Fptr)->logfilesize) )  /* EOF? */
         return(*status = END_OF_FILE);

    if (ffwhbf(fptr, &nbuff) < 0)  /* which buffer should we reuse? */
       return(*status = TOO_MANY_FILES); 

    if (dirty[nbuff])
       ffbfwt(nbuff, status); /* write dirty buffer to disk */

    if (rstart >= (fptr->Fptr)->filesize)  /* EOF? */
    {
      /* initialize an empty buffer with the correct fill value */
      if ((fptr->Fptr)->hdutype == ASCII_TBL)
         memset(iobuffer[nbuff], 32, IOBUFLEN); /* blank fill */
      else
         memset(iobuffer[nbuff],  0, IOBUFLEN);  /* zero fill */

      (fptr->Fptr)->logfilesize = maxvalue((fptr->Fptr)->logfilesize, 
              rstart + IOBUFLEN);

      dirty[nbuff] = TRUE;  /* mark record as having been modified */
    }
    else  /* not EOF, so read record from disk */
    {
      if ((fptr->Fptr)->io_pos != rstart)
           ffseek(fptr->Fptr, rstart);

      ffread(fptr->Fptr, IOBUFLEN, iobuffer[nbuff], status);
      (fptr->Fptr)->io_pos = rstart + IOBUFLEN;  /* set new IO position */
    }

    bufptr[nbuff] = fptr->Fptr;   /* file pointer for this buffer */
    bufrecnum[nbuff] = record;   /* record number contained in buffer */

updatebuf:

    (fptr->Fptr)->curbuf = nbuff; /* this is the current buffer for this file */

    if (ibuff < 0)
    { 
      /* find the current position of the buffer in the age index */
      for (ibuff = 0; ibuff < NIOBUF; ibuff++)
         if (ageindex[ibuff] == nbuff)
            break;  
    }

    /* increment the age of all the buffers that were younger than it */
    for (ibuff++; ibuff < NIOBUF; ibuff++)
      ageindex[ibuff - 1] = ageindex[ibuff];

    ageindex[NIOBUF - 1] = nbuff; /* this is now the youngest buffer */
    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffwhbf(fitsfile *fptr,        /* I - FITS file pointer             */
           int *nbuff)            /* O - which buffer to use           */
{
/*
  decide which buffer to (re)use to hold a new file record
*/
    int ii, ibuff;
    static int ageinit = 0;

    if (!ageinit)  /* first time thru, initialize default age of buffers */
    {
       for (ii = 0; ii < NIOBUF; ii++)
           ageindex[ii] = ii;
       ageinit = 1;
    }

    for (ii = 0; ii < NIOBUF; ii++)
    {
      ibuff = ageindex[ii];  /* search from the oldest to youngest buffer */

      if (bufptr[ibuff] == NULL ||         /* if buffer is empty, or    */
          bufptr[ibuff]->curbuf != ibuff)  /* is not the current buffer */
         return(*nbuff = ibuff);           /* then choose this buffer   */
    }

    /* all the buffers are locked, so we have to reuse the current one */
    /* Returns -1 if there is no current buffer (i.e. too many open files) */
    return(*nbuff = (fptr->Fptr)->curbuf);
}
/*--------------------------------------------------------------------------*/
int ffcurbuf(int nbuff,             /* i - buffer index number           */
             FITSfile **Fptr)       /* I - FITS file pointer             */
{
/*
  returns pointer to the corresponding FITSfile structure if the input
  buffer is the current I/O buffer for that FITSfile.  If it is not the
  current buffer, then it returns a null pointer.
*/
    if (bufptr[nbuff] != NULL)
    {
        if ((bufptr[nbuff])->curbuf == nbuff)
        {
             /* this is the current buffer for this file */
             *Fptr = bufptr[nbuff];
            return(0);
        }
    }

    *Fptr = NULL;
    return(0);
}
/*--------------------------------------------------------------------------*/
int ffflus(fitsfile *fptr,   /* I - FITS file pointer                       */
           int *status)      /* IO - error status                           */
/*
  Flush all the data in the current FITS file to disk. This ensures that if
  the program subsequently dies, the disk FITS file will be closed correctly.
*/
{
    int hdunum, hdutype;

    if (*status > 0)
        return(*status);

    ffghdn(fptr, &hdunum);     /* get the current HDU number */

    if (ffchdu(fptr,status) > 0)   /* close out the current HDU */
        ffpmsg("ffflus could not close the current HDU.");

    ffflsh(fptr, FALSE, status);  /* flush any modified IO buffers to disk */

    if (ffgext(fptr, hdunum - 1, &hdutype, status) > 0) /* reopen HDU */
        ffpmsg("ffflus could not reopen the current HDU.");

    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffflsh(fitsfile *fptr,        /* I - FITS file pointer           */
           int clearbuf,          /* I - also clear buffer contents? */
           int *status)           /* IO - error status               */
{
/*
  flush all dirty IO buffers associated with the file to disk
*/
    int ii;

/*
   no need to move to a different HDU

    if (fptr->HDUposition != (fptr->Fptr)->curhdu)
        ffmahd(fptr, (fptr->HDUposition) + 1, NULL, status);
*/
    for (ii = 0; ii < NIOBUF; ii++)
    {
      if (bufptr[ii] == fptr->Fptr)
      {
        if (dirty[ii])        /* flush modified buffer to disk */
           ffbfwt(ii, status);

        if (clearbuf)
          bufptr[ii] = NULL;  /* set contents of buffer as undefined */
      }
    }

    ffflushx(fptr->Fptr);  /* flush system buffers to disk */
    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffbfeof(fitsfile *fptr,        /* I - FITS file pointer           */
           int *status)           /* IO - error status               */
{
/*
  clear any buffers beyond the end of file
*/
    int ii;

    for (ii = 0; ii < NIOBUF; ii++)
    {
      if (bufptr[ii] == fptr->Fptr)
      {
        if (bufrecnum[ii] * IOBUFLEN >= fptr->Fptr->filesize)
        {
            bufptr[ii] = NULL;  /* set contents of buffer as undefined */
        }
      }
    }

    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffbfwt(int nbuff,             /* I - which buffer to write          */
           int *status)           /* IO - error status                  */
{
/*
  write contents of buffer to disk;  If the position of the buffer
  is beyond the current EOF, then the file may need to be extended
  with fill values, and/or with the contents of some of the other
  i/o buffers.
*/
    FITSfile *Fptr;
    int  ii,ibuff;
    long jj, irec, minrec, nloop, filepos;

    static char zeros[IOBUFLEN];  /*  initialized to zero by default */

    Fptr = bufptr[nbuff];
    filepos = bufrecnum[nbuff] * IOBUFLEN;

    if (filepos <= Fptr->filesize)
    {
      /* record is located within current file, so just write it */

      /* move to the correct write position */
      if (Fptr->io_pos != filepos)
         ffseek(Fptr, filepos);

      ffwrite(Fptr, IOBUFLEN, iobuffer[nbuff], status);
      Fptr->io_pos = filepos + IOBUFLEN;

      if (filepos == Fptr->filesize)   /* appended new record? */
         Fptr->filesize += IOBUFLEN;   /* increment the file size */

      dirty[nbuff] = FALSE;
    }

    else  /* if record is beyond the EOF, append any other records */ 
          /* and/or insert fill values if necessary */
    {
      /* move to EOF */
      if (Fptr->io_pos != Fptr->filesize)
         ffseek(Fptr, Fptr->filesize);

      ibuff = NIOBUF;  /* initialize to impossible value */
      while(ibuff != nbuff) /* repeat until requested buffer is written */
      {
        minrec = Fptr->filesize / IOBUFLEN;

        /* write lowest record beyond the EOF first */

        irec = bufrecnum[nbuff]; /* initially point to the requested buffer */
        ibuff = nbuff;

        for (ii = 0; ii < NIOBUF; ii++)
        {
          if (bufptr[ii] == Fptr && bufrecnum[ii] >= minrec &&
            bufrecnum[ii] < irec)
          {
            irec = bufrecnum[ii];  /* found a lower record */
            ibuff = ii;
          }
        }

        filepos = irec * IOBUFLEN;    /* byte offset of record in file */

        /* append 1 or more fill records if necessary */
        if (filepos > Fptr->filesize)
        {                    
          nloop = (filepos - (Fptr->filesize)) / IOBUFLEN; 

          for (jj = 0; jj < nloop; jj++)
            ffwrite(Fptr, IOBUFLEN, zeros, status);

          Fptr->filesize = filepos;   /* increment the file size */
        } 

        /* write the buffer itself */
        ffwrite(Fptr, IOBUFLEN, iobuffer[ibuff], status);
        dirty[ibuff] = FALSE;

        Fptr->filesize += IOBUFLEN;     /* increment the file size */
      } /* loop back if more buffers need to be written */

      Fptr->io_pos = Fptr->filesize;  /* currently positioned at EOF */
    }
    return(*status);       
}
/*--------------------------------------------------------------------------*/
int ffgrsz( fitsfile *fptr, /* I - FITS file pionter                        */
            long *ndata,    /* O - optimal amount of data to access         */
            int  *status)   /* IO - error status                            */
/*
  Returns an optimal value for the number of rows in a binary table
  or the number of pixels in an image that should be read or written
  at one time for maximum efficiency. Accessing more data than this
  may cause excessive flushing and rereading of buffers to/from disk.
*/
{
    int nfiles, typecode, bytesperpixel;
    long repeat, width;

    /* There are NIOBUF internal buffers available each IOBUFLEN bytes long. */

    if (fptr->HDUposition != (fptr->Fptr)->curhdu)
        ffmahd(fptr, (fptr->HDUposition) + 1, NULL, status);
    else if ((fptr->Fptr)->datastart == DATA_UNDEFINED)
      if ( ffrdef(fptr, status) > 0)   /* rescan header to get hdu struct */
           return(*status);

    /* determine how many different FITS files are currently open */
    nfiles = fits_get_num_files();

    /* one buffer (at least) is always allocated to each open file */

    if ((fptr->Fptr)->hdutype == IMAGE_HDU ) /* calc pixels per buffer size */
    {
      /* image pixels are in column 2 of the 'table' */
      ffgtcl(fptr, 2, &typecode, &repeat, &width, status);
      bytesperpixel = typecode / 10;
      *ndata = ((NIOBUF - nfiles) * IOBUFLEN) / bytesperpixel;
    }
    else   /* calc number of rows that fit in buffers */
    {
      *ndata = ((NIOBUF - nfiles) * IOBUFLEN) / maxvalue(1,
               (fptr->Fptr)->rowlength);
      *ndata = maxvalue(1, *ndata); 
    }

    return(*status);
}
/*--------------------------------------------------------------------------*/
int fits_get_num_files(void)
/*
  Returns the number of FITS files currently opened in CFITSIO
*/
{
    int ii, jj, unique, nfiles;

    /* determine how many different FITS files are currently open */
    nfiles = 0;
    for (ii = 0; ii < NIOBUF; ii++)
    {
      if (bufptr[ii])
      {
        unique = TRUE;

        for (jj = 0; jj < ii; jj++)
        {
          if (bufptr[ii] == bufptr[jj])
          {
            unique = FALSE;
            break;
          }
        }

        if (unique)
          nfiles++;
      }
    }
    return(nfiles);
}
/*--------------------------------------------------------------------------*/
int ffgtbb(fitsfile *fptr,        /* I - FITS file pointer                 */
           long firstrow,         /* I - starting row (1 = first row)      */
           long firstchar,        /* I - starting byte in row (1=first)    */
           long nchars,           /* I - number of bytes to read           */
           unsigned char *values, /* I - array of bytes to read            */
           int *status)           /* IO - error status                     */
/*
  read a consecutive string of bytes from an ascii or binary table.
  This will span multiple rows of the table if nchars + firstchar is
  greater than the length of a row.
*/
{
    long bytepos, endrow;

    if (*status > 0 || nchars <= 0)
        return(*status);

    else if (firstrow < 1)
        return(*status=BAD_ROW_NUM);

    else if (firstchar < 1)
        return(*status=BAD_ELEM_NUM);

    if (fptr->HDUposition != (fptr->Fptr)->curhdu)
        ffmahd(fptr, (fptr->HDUposition) + 1, NULL, status);

    /* check that we do not exceed number of rows in the table */
    endrow = ((firstchar + nchars - 2) / (fptr->Fptr)->rowlength) + firstrow;
    if (endrow > (fptr->Fptr)->numrows)
    {
        ffpmsg("attempt to read past end of table (ffgtbb)");
        return(*status=BAD_ROW_NUM);
    }

    /* move the i/o pointer to the start of the sequence of characters */
    bytepos = (fptr->Fptr)->datastart + ( (firstrow - 1) *
              (fptr->Fptr)->rowlength ) + firstchar - 1;

    ffmbyt(fptr, bytepos, REPORT_EOF, status);
    ffgbyt(fptr, nchars, values, status);  /* read the bytes */

    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffgi1b(fitsfile *fptr, /* I - FITS file pointer                         */
           long byteloc,   /* I - position within file to start reading     */
           long nvals,     /* I - number of pixels to read                  */
           long incre,     /* I - byte increment between pixels             */
           unsigned char *values, /* O - returned array of values           */
           int *status)    /* IO - error status                             */
/*
  get (read) the array of values from the FITS file, doing machine dependent
  format conversion (e.g. byte-swapping) if necessary.
*/
{
    long postemp;

    if (incre == 1)      /* read all the values at once (contiguous bytes) */
    {
        if (nvals < MINDIRECT)  /* read normally via IO buffers */
        {
           ffmbyt(fptr, byteloc, REPORT_EOF, status);
           ffgbyt(fptr, nvals, values, status);
        }
        else            /* read directly from disk, bypassing IO buffers */
        {
           postemp = (fptr->Fptr)->bytepos;   /* store current file position */
           (fptr->Fptr)->bytepos = byteloc;   /* set to the desired position */
           ffgbyt(fptr, nvals, values, status);
           (fptr->Fptr)->bytepos = postemp;   /* reset to original position */
        }
    }
    else         /* have to read each value individually (not contiguous ) */
    {
        ffmbyt(fptr, byteloc, REPORT_EOF, status);
        ffgbytoff(fptr, 1, nvals, incre - 1, values, status);
    }
    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffgi2b(fitsfile *fptr,  /* I - FITS file pointer                        */
           long byteloc,    /* I - position within file to start reading    */
           long nvals,      /* I - number of pixels to read                 */
           long incre,      /* I - byte increment between pixels            */
           short *values,   /* O - returned array of values                 */
           int *status)     /* IO - error status                            */
/*
  get (read) the array of values from the FITS file, doing machine dependent
  format conversion (e.g. byte-swapping) if necessary.
*/
{
    long postemp;

    if (incre == 2)      /* read all the values at once (contiguous bytes) */
    {
        if (nvals * 2 < MINDIRECT)  /* read normally via IO buffers */
        {
           ffmbyt(fptr, byteloc, REPORT_EOF, status);
           ffgbyt(fptr, nvals * 2, values, status);
        }
        else            /* read directly from disk, bypassing IO buffers */
        {
           postemp = (fptr->Fptr)->bytepos;   /* store current file position */
           (fptr->Fptr)->bytepos = byteloc;   /* set to the desired position */
           ffgbyt(fptr, nvals * 2, values, status);
           (fptr->Fptr)->bytepos = postemp;   /* reset to original position */
        }
    }
    else         /* have to read each value individually (not contiguous ) */
    {
        ffmbyt(fptr, byteloc, REPORT_EOF, status);
        ffgbytoff(fptr, 2, nvals, incre - 2, values, status);
    }

#if BYTESWAPPED
    ffswap2(values, nvals);    /* reverse order of bytes in each value */
#endif

    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffgi4b(fitsfile *fptr,  /* I - FITS file pointer                        */
           long byteloc,    /* I - position within file to start reading    */
           long nvals,      /* I - number of pixels to read                 */
           long incre,      /* I - byte increment between pixels            */
           INT32BIT *values, /* O - returned array of values                */
           int *status)     /* IO - error status                            */
/*
  get (read) the array of values from the FITS file, doing machine dependent
  format conversion (e.g. byte-swapping) if necessary.
*/
{
    long postemp;

    if (incre == 4)      /* read all the values at once (contiguous bytes) */
    {
        if (nvals * 4 < MINDIRECT)  /* read normally via IO buffers */
        {
           ffmbyt(fptr, byteloc, REPORT_EOF, status);
           ffgbyt(fptr, nvals * 4, values, status);
        }
        else            /* read directly from disk, bypassing IO buffers */
        {
           postemp = (fptr->Fptr)->bytepos;   /* store current file position */
           (fptr->Fptr)->bytepos = byteloc;   /* set to the desired position */
           ffgbyt(fptr, nvals * 4, values, status);
           (fptr->Fptr)->bytepos = postemp;   /* reset to original position */
        }
    }
    else         /* have to read each value individually (not contiguous ) */
    {
        ffmbyt(fptr, byteloc, REPORT_EOF, status);
        ffgbytoff(fptr, 4, nvals, incre - 4, values, status);
    }

#if BYTESWAPPED
    ffswap4(values, nvals);    /* reverse order of bytes in each value */
#endif

    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffgr4b(fitsfile *fptr,  /* I - FITS file pointer                        */
           long byteloc,    /* I - position within file to start reading    */
           long nvals,      /* I - number of pixels to read                 */
           long incre,      /* I - byte increment between pixels            */
           float *values,   /* O - returned array of values                 */
           int *status)     /* IO - error status                            */
/*
  get (read) the array of values from the FITS file, doing machine dependent
  format conversion (e.g. byte-swapping) if necessary.
*/
{
    long postemp;

    if (incre == 4)      /* read all the values at once (contiguous bytes) */
    {
        if (nvals * 4 < MINDIRECT)  /* read normally via IO buffers */
        {
           ffmbyt(fptr, byteloc, REPORT_EOF, status);
           ffgbyt(fptr, nvals * 4, values, status);
        }
        else            /* read directly from disk, bypassing IO buffers */
        {
           postemp = (fptr->Fptr)->bytepos;       /* store current file position */
           (fptr->Fptr)->bytepos = byteloc;       /* set to the desired position */
           ffgbyt(fptr, nvals * 4, values, status);
           (fptr->Fptr)->bytepos = postemp;       /* reset to original position */
        }
    }
    else         /* have to read each value individually (not contiguous ) */
    {
        ffmbyt(fptr, byteloc, REPORT_EOF, status);
        ffgbytoff(fptr, 4, nvals, incre - 4, values, status);
    }


#if MACHINE == VAXVMS
    long ii;

    ii = nvals;                      /* call VAX macro routine to convert */
    ieevur(values, values, &ii);     /* from  IEEE float -> F float       */

#elif (MACHINE == ALPHAVMS) && (FLOATTYPE == GFLOAT)
    short *sptr;

    ffswap2( (short *) values, nvals * 2);  /* swap pairs of bytes */

    /* convert from IEEE float format to VMS GFLOAT float format */
    sptr = (short *) values;
    for (ii = 0; ii < nvals; ii++, sptr += 2)
    {
        if (!fnan(*sptr) )  /* test for NaN or underflow */
            values[ii] *= 4.0;
    }

#elif BYTESWAPPED
    ffswap4((INT32BIT *)values, nvals);  /* reverse order of bytes in values */
#endif

    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffgr8b(fitsfile *fptr,  /* I - FITS file pointer                        */
           long byteloc,    /* I - position within file to start reading    */
           long nvals,      /* I - number of pixels to read                 */
           long incre,      /* I - byte increment between pixels            */
           double *values,  /* O - returned array of values                 */
           int *status)     /* IO - error status                            */
/*
  get (read) the array of values from the FITS file, doing machine dependent
  format conversion (e.g. byte-swapping) if necessary.
*/
{
    long  postemp;

    if (incre == 8)      /* read all the values at once (contiguous bytes) */
    {
        if (nvals * 8 < MINDIRECT)  /* read normally via IO buffers */
        {
           ffmbyt(fptr, byteloc, REPORT_EOF, status);
           ffgbyt(fptr, nvals * 8, values, status);
        }
        else            /* read directly from disk, bypassing IO buffers */
        {
           postemp = (fptr->Fptr)->bytepos;   /* store current file position */
           (fptr->Fptr)->bytepos = byteloc;   /* set to the desired position */
           ffgbyt(fptr, nvals * 8, values, status);
           (fptr->Fptr)->bytepos = postemp;   /* reset to original position */
        }
    }
    else         /* have to read each value individually (not contiguous ) */
    {
        ffmbyt(fptr, byteloc, REPORT_EOF, status);
        ffgbytoff(fptr, 8, nvals, incre - 8, values, status);
    }

#if MACHINE == VAXVMS
    long ii;
    ii = nvals;                      /* call VAX macro routine to convert */
    ieevud(values, values, &ii);     /* from  IEEE float -> D float       */

#elif (MACHINE == ALPHAVMS) && (FLOATTYPE == GFLOAT)
    short *sptr;
    ffswap2( (short *) values, nvals * 4);  /* swap pairs of bytes */

    /* convert from IEEE float format to VMS GFLOAT float format */
    sptr = (short *) values;
    for (ii = 0; ii < nvals; ii++, sptr += 4)
    {
        if (!dnan(*sptr) )  /* test for NaN or underflow */
            values[ii] *= 4.0;
    }

#elif BYTESWAPPED
    ffswap8(values, nvals);   /* reverse order of bytes in each value */
#endif

    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffptbb(fitsfile *fptr,        /* I - FITS file pointer                 */
           long firstrow,         /* I - starting row (1 = first row)      */
           long firstchar,        /* I - starting byte in row (1=first)    */
           long nchars,           /* I - number of bytes to write          */
           unsigned char *values, /* I - array of bytes to write           */
           int *status)           /* IO - error status                     */
/*
  write a consecutive string of bytes to an ascii or binary table.
  This will span multiple rows of the table if nchars + firstchar is
  greater than the length of a row.
*/
{
    long bytepos, endrow;

    if (*status > 0 || nchars <= 0)
        return(*status);

    else if (firstrow < 1)
        return(*status=BAD_ROW_NUM);

    else if (firstchar < 1)
        return(*status=BAD_ELEM_NUM);

    if (fptr->HDUposition != (fptr->Fptr)->curhdu)
        ffmahd(fptr, (fptr->HDUposition) + 1, NULL, status);
    else if ((fptr->Fptr)->datastart < 0) /* rescan header if data undefined */
        ffrdef(fptr, status);

    /* move the i/o pointer to the start of the sequence of characters */
    bytepos = (fptr->Fptr)->datastart + ( (firstrow - 1) * 
              (fptr->Fptr)->rowlength ) + firstchar - 1;

    ffmbyt(fptr, bytepos, IGNORE_EOF, status);
    ffpbyt(fptr, nchars, values, status);  /* write the bytes */

    /* update number of rows in the table if necessary */

    endrow = ((firstchar + nchars - 2) / (fptr->Fptr)->rowlength) + firstrow;
    if (endrow > (fptr->Fptr)->numrows)
        (fptr->Fptr)->numrows = endrow;

    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffpi1b(fitsfile *fptr, /* I - FITS file pointer                         */
           long nvals,     /* I - number of pixels in the values array      */
           long incre,     /* I - byte increment between pixels             */
           unsigned char *values, /* I - array of values to write           */
           int *status)    /* IO - error status                             */
/*
  put (write) the array of values to the FITS file, doing machine dependent
  format conversion (e.g. byte-swapping) if necessary.
*/
{
    if (incre == 1)      /* write all the values at once (contiguous bytes) */

        ffpbyt(fptr, nvals, values, status);

    else         /* have to write each value individually (not contiguous ) */

        ffpbytoff(fptr, 1, nvals, incre - 1, values, status);

    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffpi2b(fitsfile *fptr, /* I - FITS file pointer                         */
           long nvals,     /* I - number of pixels in the values array      */
           long incre,     /* I - byte increment between pixels             */
           short *values,  /* I - array of values to write                  */
           int *status)    /* IO - error status                             */
/*
  put (write) the array of values to the FITS file, doing machine dependent
  format conversion (e.g. byte-swapping) if necessary.
*/
{
#if BYTESWAPPED
    ffswap2(values, nvals);  /* reverse order of bytes in each value */
#endif

    if (incre == 2)      /* write all the values at once (contiguous bytes) */

        ffpbyt(fptr, nvals * 2, values, status);

    else         /* have to write each value individually (not contiguous ) */

        ffpbytoff(fptr, 2, nvals, incre - 2, values, status);

    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffpi4b(fitsfile *fptr, /* I - FITS file pointer                         */
           long nvals,     /* I - number of pixels in the values array      */
           long incre,     /* I - byte increment between pixels             */
           INT32BIT *values, /* I - array of values to write                */
           int *status)    /* IO - error status                             */
/*
  put (write) the array of values to the FITS file, doing machine dependent
  format conversion (e.g. byte-swapping) if necessary.
*/
{
#if BYTESWAPPED
    ffswap4(values, nvals);    /* reverse order of bytes in each value */
#endif

    if (incre == 4)      /* write all the values at once (contiguous bytes) */

        ffpbyt(fptr, nvals * 4, values, status);

    else         /* have to write each value individually (not contiguous ) */

        ffpbytoff(fptr, 4, nvals, incre - 4, values, status);

    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffpr4b(fitsfile *fptr, /* I - FITS file pointer                         */
           long nvals,     /* I - number of pixels in the values array      */
           long incre,     /* I - byte increment between pixels             */
           float *values,  /* I - array of values to write                  */
           int *status)    /* IO - error status                             */
/*
  put (write) the array of values to the FITS file, doing machine dependent
  format conversion (e.g. byte-swapping) if necessary.
*/
{
#if MACHINE == VAXVMS
    long ii;

    ii = nvals;                      /* call VAX macro routine to convert */
    ieevpr(values, values, &ii);     /* from F float -> IEEE float        */

#elif (MACHINE == ALPHAVMS) && (FLOATTYPE == GFLOAT)
    long ii;

    /* convert from VMS FFLOAT float format to IEEE float format */
    for (ii = 0; ii < nvals; ii++)
        values[ii] *= 0.25;

    ffswap2( (short *) values, nvals * 2);  /* swap pairs of bytes */

#elif BYTESWAPPED
    ffswap4((INT32BIT *) values, nvals); /* reverse order of bytes in values */
#endif

    if (incre == 4)      /* write all the values at once (contiguous bytes) */

        ffpbyt(fptr, nvals * 4, values, status);

    else         /* have to write each value individually (not contiguous ) */

        ffpbytoff(fptr, 4, nvals, incre - 4, values, status);

    return(*status);
}
/*--------------------------------------------------------------------------*/
int ffpr8b(fitsfile *fptr, /* I - FITS file pointer                         */
           long nvals,     /* I - number of pixels in the values array      */
           long incre,     /* I - byte increment between pixels             */
           double *values, /* I - array of values to write                  */
           int *status)    /* IO - error status                             */
/*
  put (write) the array of values to the FITS file, doing machine dependent
  format conversion (e.g. byte-swapping) if necessary.
*/
{
#if MACHINE == VAXVMS
    long ii;

    ii = nvals;                      /* call VAX macro routine to convert */
    ieevpd(values, values, &ii);     /* from D float -> IEEE float        */

#elif (MACHINE == ALPHAVMS) && (FLOATTYPE == GFLOAT)
    long ii;

    /* convert from VMS GFLOAT float format to IEEE float format */
    for (ii = 0; ii < nvals; ii++)
        values[ii] *= 0.25;

    ffswap2( (short *) values, nvals * 4);  /* swap pairs of bytes */

#elif BYTESWAPPED
    ffswap8(values, nvals); /* reverse order of bytes in each value */
#endif

    if (incre == 8)      /* write all the values at once (contiguous bytes) */

        ffpbyt(fptr, nvals * 8, values, status);

    else         /* have to write each value individually (not contiguous ) */

        ffpbytoff(fptr, 8, nvals, incre - 8, values, status);

    return(*status);
}

