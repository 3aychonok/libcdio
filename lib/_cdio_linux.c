/*
    $Id: _cdio_linux.c,v 1.1.1.1 2003/03/24 19:01:09 rocky Exp $

    Copyright (C) 2001 Herbert Valerio Riedel <hvr@gnu.org>
    Copyright (C) 2002,2003 Rocky Bernstein <rocky@panix.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* This file contains Linux-specific code and implements low-level 
   control of the CD drive.
*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

static const char _rcsid[] = "$Id: _cdio_linux.c,v 1.1.1.1 2003/03/24 19:01:09 rocky Exp $";

#include "cdio_assert.h"
#include "cdio_private.h"
#include "sector.h"
#include "util.h"

#ifdef HAVE_LINUX_CDROM

#if defined(HAVE_LINUX_VERSION_H)
# include <linux/version.h>
# if LINUX_VERSION_CODE >= KERNEL_VERSION(2,2,16)
#   define __CDIO_LINUXCD_BUILD
# else
#  error "You need a kernel greater than 2.2.16 to have CDROM support"
# endif
#else 
#  error "You need <linux/version.h> to have CDROM support"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <linux/cdrom.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#define DEFAULT_CDIO_DEVICE "/dev/cdrom"

#define TOTAL_TRACKS    (_obj->tochdr.cdth_trk1)
#define FIRST_TRACK_NUM (_obj->tochdr.cdth_trk0)

typedef struct {
  int fd;

  int ioctls_debugged; /* for debugging */

  enum {
    _AM_NONE,
    _AM_IOCTL,
    _AM_READ_CD,
    _AM_READ_10
  } access_mode;

  char *source_name;
  
  bool init;

  /* Track information */
  bool toc_init;                         /* if true, info below is valid. */
  struct cdrom_tochdr    tochdr;
  struct cdrom_tocentry  tocent[100];    /* entry info for each track */

} _img_private_t;

/*!
  Initialize CD device.
 */
static bool
_cdio_init (_img_private_t *_obj)
{
  if (_obj->init) {
    cdio_error ("init called more than once");
    return false;
  }
  
  _obj->fd = open (_obj->source_name, O_RDONLY, 0);

  if (_obj->fd < 0)
    {
      cdio_error ("open (%s): %s", _obj->source_name, strerror (errno));
      return false;
    }

  _obj->init = true;
  _obj->toc_init = false;
  return true;
}

/*!
  Release and free resources associated with cd. 
 */
static void
_cdio_free (void *user_data)
{
  _img_private_t *_obj = user_data;

  if (NULL == _obj) return;
  free (_obj->source_name);

  if (_obj->fd >= 0)
    close (_obj->fd);

  free (_obj);
}

static int 
_set_bsize (int fd, unsigned bsize)
{
  struct cdrom_generic_command cgc;

  struct
  {
    uint8_t reserved1;
    uint8_t medium;
    uint8_t reserved2;
    uint8_t block_desc_length;
    uint8_t density;
    uint8_t number_of_blocks_hi;
    uint8_t number_of_blocks_med;
    uint8_t number_of_blocks_lo;
    uint8_t reserved3;
    uint8_t block_length_hi;
    uint8_t block_length_med;
    uint8_t block_length_lo;
  } mh;

  memset (&mh, 0, sizeof (mh));
  memset (&cgc, 0, sizeof (struct cdrom_generic_command));
  
  cgc.cmd[0] = 0x15;
  cgc.cmd[1] = 1 << 4;
  cgc.cmd[4] = 12;
  
  cgc.buflen = sizeof (mh);
  cgc.buffer = (void *) &mh;

  cgc.data_direction = CGC_DATA_WRITE;

  mh.block_desc_length = 0x08;
  mh.block_length_hi = (bsize >> 16) & 0xff;
  mh.block_length_med = (bsize >> 8) & 0xff;
  mh.block_length_lo = (bsize >> 0) & 0xff;

  return ioctl (fd, CDROM_SEND_PACKET, &cgc);
}

static int
__read_mode2 (int fd, void *buf, lba_t lba, unsigned nblocks, 
		 bool _workaround)
{
  struct cdrom_generic_command cgc;

  memset (&cgc, 0, sizeof (struct cdrom_generic_command));

  cgc.cmd[0] = _workaround ? GPCMD_READ_10 : GPCMD_READ_CD;
  
  cgc.cmd[2] = (lba >> 24) & 0xff;
  cgc.cmd[3] = (lba >> 16) & 0xff;
  cgc.cmd[4] = (lba >> 8) & 0xff;
  cgc.cmd[5] = (lba >> 0) & 0xff;

  cgc.cmd[6] = (nblocks >> 16) & 0xff;
  cgc.cmd[7] = (nblocks >> 8) & 0xff;
  cgc.cmd[8] = (nblocks >> 0) & 0xff;

  if (!_workaround)
    {
      cgc.cmd[1] = 0; /* sector size mode2 */

      cgc.cmd[9] = 0x58; /* 2336 mode2 */
    }

  cgc.buflen = 2336 * nblocks;
  cgc.buffer = buf;

#ifdef HAVE_LINUX_CDROM_TIMEOUT
  cgc.timeout = 500;
#endif
  cgc.data_direction = CGC_DATA_READ;

  if (_workaround)
    {
      int retval;

      if ((retval = _set_bsize (fd, 2336)))
	return retval;

      if ((retval = ioctl (fd, CDROM_SEND_PACKET, &cgc)))
	{
	  _set_bsize (fd, 2048);
	  return retval;
	}

      if ((retval = _set_bsize (fd, 2048)))
	return retval;
    }
  else
    return ioctl (fd, CDROM_SEND_PACKET, &cgc);

  return 0;
}

static int
_read_mode2 (int fd, void *buf, lba_t lba, unsigned nblocks, 
	     bool _workaround)
{
  unsigned l = 0;
  int retval = 0;

  while (nblocks > 0)
    {
      const unsigned nblocks2 = (nblocks > 25) ? 25 : nblocks;
      void *buf2 = ((char *)buf ) + (l * 2336);
      
      retval |= __read_mode2 (fd, buf2, lba + l, nblocks2, _workaround);

      if (retval)
	break;

      nblocks -= nblocks2;
      l += nblocks2;
    }

  return retval;
}

/*!
   Reads a single mode2 sector from cd device into data starting
   from lsn. Returns 0 if no error. 
 */
static int
_read_mode2_sector (void *user_data, void *data, lsn_t lsn, 
		    bool mode2_form2)
{
  char buf[M2RAW_SECTOR_SIZE] = { 0, };
  struct cdrom_msf *msf = (struct cdrom_msf *) &buf;
  msf_t _msf;

  _img_private_t *_obj = user_data;

  cdio_lba_to_msf (cdio_lsn_to_lba(lsn), &_msf);
  msf->cdmsf_min0 = from_bcd8(_msf.m);
  msf->cdmsf_sec0 = from_bcd8(_msf.s);
  msf->cdmsf_frame0 = from_bcd8(_msf.f);

  if (_obj->ioctls_debugged == 75)
    cdio_debug ("only displaying every 75th ioctl from now on");

  if (_obj->ioctls_debugged == 30 * 75)
    cdio_debug ("only displaying every 30*75th ioctl from now on");
  
  if (_obj->ioctls_debugged < 75 
      || (_obj->ioctls_debugged < (30 * 75)  
	  && _obj->ioctls_debugged % 75 == 0)
      || _obj->ioctls_debugged % (30 * 75) == 0)
    cdio_debug ("reading %2.2d:%2.2d:%2.2d",
	       msf->cdmsf_min0, msf->cdmsf_sec0, msf->cdmsf_frame0);
  
  _obj->ioctls_debugged++;
 
 retry:
  switch (_obj->access_mode)
    {
    case _AM_NONE:
      cdio_error ("no way to read mode2");
      return 1;
      break;
      
    case _AM_IOCTL:
      if (ioctl (_obj->fd, CDROMREADMODE2, &buf) == -1)
	{
	  perror ("ioctl()");
	  return 1;
	  /* exit (EXIT_FAILURE); */
	}
      break;
      
    case _AM_READ_CD:
    case _AM_READ_10:
      if (_read_mode2 (_obj->fd, buf, lsn, 1, 
		       (_obj->access_mode == _AM_READ_10)))
	{
	  perror ("ioctl()");
	  if (_obj->access_mode == _AM_READ_CD)
	    {
	      cdio_info ("READ_CD failed; switching to READ_10 mode...");
	      _obj->access_mode = _AM_READ_10;
	      goto retry;
	    }
	  else
	    {
	      cdio_info ("READ_10 failed; switching to ioctl(CDROMREADMODE2) mode...");
	      _obj->access_mode = _AM_IOCTL;
	      goto retry;
	    }
	  return 1;
	}
      break;
    }

  if (mode2_form2)
    memcpy (data, buf, M2RAW_SECTOR_SIZE);
  else
    memcpy (((char *)data), buf + 8, M2F1_SECTOR_SIZE);
  
  return 0;
}

/*!
   Reads nblocks of mode2 sectors from cd device into data starting
   from lsn.
   Returns 0 if no error. 
 */
static int
_read_mode2_sectors (void *user_data, void *data, lsn_t lsn, 
		     bool mode2_form2, unsigned nblocks)
{
  _img_private_t *_obj = user_data;
  int i;
  int retval;

  for (i = 0; i < nblocks; i++) {
    if (mode2_form2) {
      if ( (retval = _read_mode2_sector (_obj, 
					  ((char *)data) + (M2RAW_SECTOR_SIZE * i),
					  lsn + i, true)) )
	return retval;
    } else {
      char buf[M2RAW_SECTOR_SIZE] = { 0, };
      if ( (retval = _read_mode2_sector (_obj, buf, lsn + i, true)) )
	return retval;
      
      memcpy (((char *)data) + (M2F1_SECTOR_SIZE * i), buf + 8, 
	      M2F1_SECTOR_SIZE);
    }
  }
  return 0;
}

/*!
   Return the size of the CD in logical block address (LBA) units.
 */
static uint32_t 
_cdio_stat_size (void *user_data)
{
  _img_private_t *_obj = user_data;

  struct cdrom_tocentry tocent;
  uint32_t size;

  tocent.cdte_track = CDROM_LEADOUT;
  tocent.cdte_format = CDROM_LBA;
  if (ioctl (_obj->fd, CDROMREADTOCENTRY, &tocent) == -1)
    {
      perror ("ioctl(CDROMREADTOCENTRY)");
      exit (EXIT_FAILURE);
    }

  size = tocent.cdte_addr.lba;

  return size;
}

/*!
  Set the key "arg" to "value" in source device.
*/
static int
_cdio_set_arg (void *user_data, const char key[], const char value[])
{
  _img_private_t *_obj = user_data;

  if (!strcmp (key, "source"))
    {
      if (!value)
	return -2;

      free (_obj->source_name);
      
      _obj->source_name = strdup (value);
    }
  else if (!strcmp (key, "access-mode"))
    {
      if (!strcmp(value, "IOCTL"))
	_obj->access_mode = _AM_IOCTL;
      else if (!strcmp(value, "READ_CD"))
	_obj->access_mode = _AM_READ_CD;
      else if (!strcmp(value, "READ_10"))
	_obj->access_mode = _AM_READ_10;
      else
	cdio_error ("unknown access type: %s. ignored.", value);
    }
  else 
    return -1;

  return 0;
}

/*! 
  Read and cache the CD's Track Table of Contents and track info.
  Return false if successful or true if an error.
*/
static bool
_cdio_read_toc (_img_private_t *_obj) 
{
  int i;

  /* read TOC header */
  if ( ioctl(_obj->fd, CDROMREADTOCHDR, &_obj->tochdr) == -1 ) {
    cdio_error("%s: %s\n", 
            "error in ioctl CDROMREADTOCHDR", strerror(errno));
    return false;
  }

  /* read individual tracks */
  for (i= FIRST_TRACK_NUM; i<=TOTAL_TRACKS; i++) {
    _obj->tocent[i-1].cdte_track = i;
    _obj->tocent[i-1].cdte_format = CDROM_MSF;
    if ( ioctl(_obj->fd, CDROMREADTOCENTRY, &_obj->tocent[i-1]) == -1 ) {
      cdio_error("%s %d: %s\n",
              "error in ioctl CDROMREADTOCENTRY for track", 
              i, strerror(errno));
      return false;
    }
    /****
    struct cdrom_msf0 *msf= &_obj->tocent[i-1].cdte_addr.msf;
    
    fprintf (stdout, "--- track# %d (msf %2.2x:%2.2x:%2.2x)\n",
	     i, msf->minute, msf->second, msf->frame);
    ****/

  }

  /* read the lead-out track */
  _obj->tocent[TOTAL_TRACKS].cdte_track = CDROM_LEADOUT;
  _obj->tocent[TOTAL_TRACKS].cdte_format = CDROM_MSF;

  if (ioctl(_obj->fd, CDROMREADTOCENTRY, 
	    &_obj->tocent[TOTAL_TRACKS]) == -1 ) {
    cdio_error("%s: %s\n", 
	     "error in ioctl CDROMREADTOCENTRY for lead-out",
            strerror(errno));
    return false;
  }

  /*
  struct cdrom_msf0 *msf= &_obj->tocent[TOTAL_TRACKS].cdte_addr.msf;

  fprintf (stdout, "--- track# %d (msf %2.2x:%2.2x:%2.2x)\n",
	   i, msf->minute, msf->second, msf->frame);
  */

  return true;
}

/*!
  Eject media in CD drive. If successful, as a side effect we 
  also free obj.
 */
static int 
_cdio_eject_media (void *user_data) {

  _img_private_t *_obj = user_data;
  int ret, status;

  if (_obj->fd > -1) {
    if((status = ioctl(_obj->fd, CDROM_DRIVE_STATUS, CDSL_CURRENT)) > 0) {
      switch(status) {
      case CDS_TRAY_OPEN:
	if((ret = ioctl(_obj->fd, CDROMCLOSETRAY)) != 0) {
	  cdio_error ("CDROMCLOSETRAY failed: %s\n", strerror(errno));  
	}
	break;
      case CDS_DISC_OK:
	if((ret = ioctl(_obj->fd, CDROMEJECT)) != 0) {
	  cdio_error("CDROMEJECT failed: %s\n", strerror(errno));  
	}
	break;
      }
      _cdio_free((void *) _obj);
      return 0;
    } else {
      cdio_error ("CDROM_DRIVE_STATUS failed: %s\n", strerror(errno));
      _cdio_free((void *) _obj);
      return 1;
    }
  }
  return 2;
}

/*!
  Return the value associated with the key "arg".
*/
static const char *
_cdio_get_arg (void *user_data, const char key[])
{
  _img_private_t *_obj = user_data;

  if (!strcmp (key, "source")) {
    return _obj->source_name;
  } else if (!strcmp (key, "access-mode")) {
    switch (_obj->access_mode) {
    case _AM_IOCTL:
      return "ioctl";
    case _AM_READ_CD:
      return "READ_CD";
    case _AM_READ_10:
      return "READ_10";
    case _AM_NONE:
      return "no access method";
    }
  } 
  return NULL;
}

/*!
  Return a string containing the default VCD device if none is specified.
 */
static char *
_cdio_get_default_device()
{
  return strdup(DEFAULT_CDIO_DEVICE);
}

/*!
  Return the number of of the first track. 
  CDIO_INVALID_TRACK is returned on error.
*/
static track_t
_cdio_get_first_track_num(void *user_data) 
{
  _img_private_t *_obj = user_data;
  
  if (!_obj->toc_init) _cdio_read_toc (_obj) ;

  return FIRST_TRACK_NUM;
}

/*!
  Return the number of tracks in the current medium.
  CDIO_INVALID_TRACK is returned on error.
*/
static track_t
_cdio_get_num_tracks(void *user_data) 
{
  _img_private_t *_obj = user_data;
  
  if (!_obj->toc_init) _cdio_read_toc (_obj) ;

  return TOTAL_TRACKS;
}

/*!  
  Get format of track. 
*/
static track_format_t
_cdio_get_track_format(void *user_data, track_t track_num) 
{
  _img_private_t *_obj = user_data;
  
  if (!_obj->toc_init) _cdio_read_toc (_obj) ;

  if (track_num > TOTAL_TRACKS || track_num == 0)
    return TRACK_FORMAT_ERROR;

  /* This is pretty much copied from the "badly broken" cdrom_count_tracks
     in linux/cdrom.c.
   */
  if (_obj->tocent[track_num-1].cdte_ctrl & CDROM_DATA_TRACK) {
    if (_obj->tocent[track_num-1].cdte_format == 0x10)
      return TRACK_FORMAT_CDI;
    else if (_obj->tocent[track_num-1].cdte_format == 0x20) 
      return TRACK_FORMAT_XA;
    else
      return TRACK_FORMAT_DATA;
  } else
    return TRACK_FORMAT_AUDIO;
  
}

/*!
  Return true if we have XA data (green, mode2 form1) or
  XA data (green, mode2 form2). That is track begins:
  sync - header - subheader
  12     4      -  8

  FIXME: there's gotta be a better design for this and get_track_format?
*/
static bool
_cdio_get_track_green(void *user_data, track_t track_num) 
{
  _img_private_t *_obj = user_data;
  
  if (!_obj->toc_init) _cdio_read_toc (_obj) ;

  if (track_num == CDIO_LEADOUT_TRACK) track_num = TOTAL_TRACKS+1;

  if (track_num > TOTAL_TRACKS+1 || track_num == 0)
    return false;

  /* FIXME: Dunno if this is the right way, but it's what 
     I was using in cdinfo for a while.
   */
  return ((_obj->tocent[track_num-1].cdte_ctrl & 2) != 0);
}

/*!  
  Return the starting MSF (minutes/secs/frames) for track number
  track_num in obj.  Track numbers start at 1.
  The "leadout" track is specified either by
  using track_num LEADOUT_TRACK or the total tracks+1.
  False is returned if there is no track entry.
*/
static bool
_cdio_get_track_msf(void *user_data, track_t track_num, msf_t *msf)
{
  _img_private_t *_obj = user_data;

  if (NULL == msf) return false;

  if (!_obj->toc_init) _cdio_read_toc (_obj) ;

  if (track_num == CDIO_LEADOUT_TRACK) track_num = TOTAL_TRACKS+1;

  if (track_num > TOTAL_TRACKS+1 || track_num == 0) {
    return false;
  } else {
    struct cdrom_msf0  *msf0= &_obj->tocent[track_num-1].cdte_addr.msf;
    msf->m = to_bcd8(msf0->minute);
    msf->s = to_bcd8(msf0->second);
    msf->f = to_bcd8(msf0->frame);
    return true;
  }
}

#endif /* HAVE_LINUX_CDROM */

/*!
  Initialization routine. This is the only thing that doesn't
  get called via a function pointer. In fact *we* are the
  ones to set that up.
 */
CdIo *
cdio_open_linux (const char *source_name)
{

#ifdef HAVE_LINUX_CDROM
  CdIo *ret;
  _img_private_t *_data;

  cdio_funcs _funcs = {
    .eject_media        = _cdio_eject_media,
    .free               = _cdio_free,
    .get_arg            = _cdio_get_arg,
    .get_default_device = _cdio_get_default_device,
    .get_first_track_num= _cdio_get_first_track_num,
    .get_num_tracks     = _cdio_get_num_tracks,
    .get_track_format   = _cdio_get_track_format,
    .get_track_green    = _cdio_get_track_green,
    .get_track_lba      = NULL, /* This could be implemented if need be. */
    .get_track_msf      = _cdio_get_track_msf,
    .read_mode2_sector  = _read_mode2_sector,
    .read_mode2_sectors = _read_mode2_sectors,
    .set_arg            = _cdio_set_arg,
    .stat_size          = _cdio_stat_size
  };

  _data                 = _cdio_malloc (sizeof (_img_private_t));
  _data->access_mode    = _AM_READ_CD;
  _data->init           = false;
  _data->fd             = -1;

  _cdio_set_arg(_data, "source", (NULL == source_name) 
		? DEFAULT_CDIO_DEVICE: source_name);

  ret = cdio_new (_data, &_funcs);
  if (ret == NULL) return NULL;

  if (_cdio_init(_data))
    return ret;
  else {
    _cdio_free (_data);
    return NULL;
  }
  
#else 
  return NULL;
#endif /* HAVE_LINUX_CDROM */

}

bool
cdio_have_linux (void)
{
#ifdef HAVE_LINUX_CDROM
  return true;
#else 
  return false;
#endif /* HAVE_LINUX_CDROM */
}

