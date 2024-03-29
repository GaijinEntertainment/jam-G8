/*
 * Copyright 1993-2002 Christopher Seiwald and Perforce Software, Inc.
 *
 * This file is part of Jam - see jam.c for Copyright information.
 */

/*
 * filent.c - scan directories and archives on NT
 *
 * External routines:
 *
 *	file_dirscan() - scan a directory for files
 *	file_time() - get timestamp of file, if not done by file_dirscan()
 *	file_archscan() - scan an archive for files
 *
 * File_dirscan() and file_archscan() call back a caller provided function
 * for each file found.  A flag to this callback function lets file_dirscan()
 * and file_archscan() indicate that a timestamp is being provided with the
 * file.   If file_dirscan() or file_archscan() do not provide the file's
 * timestamp, interested parties may later call file_time().
 *
 * 07/10/95 (taylor)  Findfirst() returns the first file on NT.
 * 05/03/96 (seiwald) split apart into pathnt.c
 * 01/20/00 (seiwald) - Upgraded from K&R to ANSI C
 * 10/03/00 (anton) - Porting for Borland C++ 5.5
 * 01/08/01 (seiwald) - closure param for file_dirscan/file_archscan
 * 11/04/02 (seiwald) - const-ing for string literals
 * 01/23/03 (seiwald) - long long handles for NT IA64
 */

# include "jam.h"
# include "filesys.h"
# include "pathsys.h"

# ifdef OS_NT

# ifdef __BORLANDC__
# if __BORLANDC__ < 0x550
# include <dir.h>
# include <dos.h>
# endif
# undef PATHNAME	/* cpp namespace collision */
# define _finddata_t ffblk
# endif

# include <io.h>
# include <sys/stat.h>
#ifndef __DMC__
  #include <direct.h>
#endif

#ifdef DOS386
# include <dos.h>
# include <direct.h>
#endif

/*
 * file_dirscan() - scan a directory for files
 */

# if defined( __ia64__ ) || \
     defined( __IA64__ ) || \
     defined( _M_IA64 ) || \
     defined(_M_AMD64) || defined(_M_X64) || defined(_M_ARM64) || INTPTR_MAX != INT32_MAX
# define FINDTYPE long long
# else
# define FINDTYPE long
# endif

void
file_dirscan( 
	const char *dir,
	scanback func,
	void	*closure )
{
	PATHNAME f;
	char filespec[ MAXJPATH ];
	char filename[ MAXJPATH ];
	FINDTYPE handle;
	int ret;
	struct _finddata_t finfo[1];

	/* First enter directory itself */

	memset( (char *)&f, '\0', sizeof( f ) );

	f.f_dir.ptr = dir;
	f.f_dir.len = (int)strlen(dir);

	dir = *dir ? dir : ".";

 	/* Special case \ or d:\ : enter it */
 
 	if( f.f_dir.len == 1 && f.f_dir.ptr[0] == '\\' )
 	    (*func)( closure, dir, 0 /* not stat()'ed */, (time_t)0 );
 	else if( f.f_dir.len == 3 && f.f_dir.ptr[1] == ':' )
 	    (*func)( closure, dir, 0 /* not stat()'ed */, (time_t)0 );

	/* Now enter contents of directory */

	sprintf( filespec, "%s/*", dir );

	if( DEBUG_BINDSCAN )
	    printf( "scan directory %s\n", dir );

# if defined(__BORLANDC__) && __BORLANDC__ < 0x550
	if ( ret = findfirst( filespec, finfo, FA_NORMAL | FA_DIREC ) )
	    return;

	while( !ret )
	{
	    time_t time_write = finfo->ff_fdate;

	    time_write = (time_write << 16) | finfo->ff_ftime;
	    f.f_base.ptr = finfo->ff_name;
	    f.f_base.len = strlen( finfo->ff_name );

	    path_build( &f, filename );

	    (*func)( closure, filename, 1 /* stat()'ed */, time_write );

	    ret = findnext( finfo );
	}
# elif !defined(DOS386)
	handle = _findfirst( filespec, finfo );

	if( ret = ( handle == (FINDTYPE)(-1) ) )
	    return;

	while( !ret )
	{
	    f.f_base.ptr = finfo->name;
	    f.f_base.len = (int)strlen( finfo->name );

	    path_build( &f, filename, 0 );

	    (*func)( closure, filename, 1 /* stat()'ed */, finfo->time_write );

	    ret = _findnext( handle, finfo );
	}

	_findclose( handle );
# else
  struct time_format { 
    unsigned two_seconds: 5;
    unsigned minutes: 6; 
    unsigned hours: 5;
  } time;
  struct date_format { 
    unsigned day: 5;
    unsigned month: 4; 
    unsigned year: 7;
  } date;
  struct tm ntime;

  strcat ( filespec, ".*" );
  struct FIND *block = findfirst ( filespec, _A_SUBDIR );
  while( block )
  {
      if ((block->attribute & _A_SUBDIR) &&
          (strcmp ( block->name, "." ) == 0 ||
           strcmp ( block->name, ".." ) == 0 )) {
        block = findnext();
        continue;
      }

      time = *(struct time_format *) &block->time;
      date = *(struct date_format *) &block->date;

      ntime.tm_sec = time.two_seconds*2;
      ntime.tm_min = time.minutes;
      ntime.tm_hour = time.hours;
      ntime.tm_mday = date.day;
      ntime.tm_mon = date.month-1;
      ntime.tm_year = date.year+80;
      ntime.tm_wday = 0;
      ntime.tm_yday = 0;
      ntime.tm_isdst = 0;

      unsigned f_time = mktime ( &ntime );
      //printf ( "file %16s %s", block->name, asctime(&ntime));

      f.f_base.ptr = block->name;
      f.f_base.len = strlen( block->name );

      path_build( &f, filename, 0 );

      (*func)( closure, filename, 1 /* stat()'ed */, f_time );

      block = findnext();
  }
# endif

}

/*
 * file_time() - get timestamp of file, if not done by file_dirscan()
 */

int
file_time(
	const char *filename,
	time_t	*time )
{
	/* On NT this is called only for C:/ */

	struct stat statbuf;

	if( stat( filename, &statbuf ) < 0 )
	    return -1;

	*time = statbuf.st_mtime;

	return 0;
}

/*
 * file_archscan() - scan an archive for files
 */

/* Straight from SunOS */

#define	ARMAG	"!<arch>\n"
#define	SARMAG	8

#define	ARFMAG	"`\n"

struct ar_hdr {
	char	ar_name[16];
	char	ar_date[12];
	char	ar_uid[6];
	char	ar_gid[6];
	char	ar_mode[8];
	char	ar_size[10];
	char	ar_fmag[2];
};

# define SARFMAG 2
# define SARHDR sizeof( struct ar_hdr )

void
file_archscan(
	const char *archive,
	scanback func,
	void	*closure )
{
	struct ar_hdr ar_hdr;
	char *string_table = 0;
	char buf[ MAXJPATH ];
	long offset;
	int fd;

	if( ( fd = open( archive, O_RDONLY | O_BINARY, 0 ) ) < 0 )
	    return;

	if( read( fd, buf, SARMAG ) != SARMAG ||
	    strncmp( ARMAG, buf, SARMAG ) )
	{
	    close( fd );
	    return;
	}

	offset = SARMAG;

	if( DEBUG_BINDSCAN )
	    printf( "scan archive %s\n", archive );

	while( read( fd, &ar_hdr, SARHDR ) == SARHDR &&
	       !memcmp( ar_hdr.ar_fmag, ARFMAG, SARFMAG ) )
	{
	    long    lar_date;
	    long    lar_size;
	    char    *name = 0;
 	    char    *endname;
	    char    *c;

	    sscanf( ar_hdr.ar_date, "%ld", &lar_date );
	    sscanf( ar_hdr.ar_size, "%ld", &lar_size );

	    lar_size = ( lar_size + 1 ) & ~1;

	    if (ar_hdr.ar_name[0] == '/' && ar_hdr.ar_name[1] == '/' )
	    {
		/* this is the "string table" entry of the symbol table,
		** which holds strings of filenames that are longer than
		** 15 characters (ie. don't fit into a ar_name
		*/

		string_table = malloc(lar_size);
		if (read(fd, string_table, lar_size) != lar_size)
		    printf("error reading string table\n");
		offset += SARHDR + lar_size;
		continue;
	    }
	    else if (ar_hdr.ar_name[0] == '/' && ar_hdr.ar_name[1] != ' ')
	    {
		/* Long filenames are recognized by "/nnnn" where nnnn is
		** the offset of the string in the string table represented
		** in ASCII decimals.
		*/

		name = string_table + atoi( ar_hdr.ar_name + 1 );
		endname = name + strlen( name );
	    }
	    else
	    {
		/* normal name */
		name = ar_hdr.ar_name;
		endname = name + sizeof( ar_hdr.ar_name );
	    }

	    /* strip trailing space, slashes, and backslashes */

	    while( endname-- > name )
		if( *endname != ' ' && *endname != '\\' && *endname != '/' )
		    break;
	    *++endname = 0;

	    /* strip leading directory names, an NT specialty */

	    if( c = strrchr( name, '/' ) )
		name = c + 1;
	    if( c = strrchr( name, '\\' ) )
		name = c + 1;

	    sprintf( buf, "%s(%.*s)", archive, (int)(endname - name), name );
	    (*func)( closure, buf, 1 /* time valid */, (time_t)lar_date );

	    offset += SARHDR + lar_size;
	    lseek( fd, offset, 0 );
	}

	close( fd );
}

const char *file_simplify_rel_path(const char *path, char *stor, int stor_len)
{
  static char cur_dir[260] = "*";
  static unsigned short cd_num_subfolder = 0, cd_subfolder_pos[32] = {0};
  if (!path || !*path || path[0] != '.' || path[1] != '.')
    return path;

  if (cur_dir[0] == '*')
  {
    char *p = cur_dir;
    getcwd(cur_dir, sizeof(cur_dir));
    for (; *p; p++)
      if (*p == '\\')
        *p = '/';
    for (p = cur_dir+strlen(cur_dir)-1; p > cur_dir; p--)
      if (*p == '/')
        cd_subfolder_pos[cd_num_subfolder++] = p+1 - cur_dir;
  }

  int subdirs = -1;
  while (*path)
    if (subdirs + 1 < cd_num_subfolder && path[0] == '.' && path[1] == '.' && (path[2] == '/' || path[2] == '\\'))
      subdirs ++, path += 3;
    else
      break;
  if (subdirs < 0)
    return path;
  _snprintf(stor, stor_len, "%.*s%s", cd_subfolder_pos[subdirs], cur_dir, path);
  return stor;
}

# endif /* NT */
