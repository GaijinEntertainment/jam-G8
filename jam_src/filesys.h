/*
 * Copyright 1993-2002 Christopher Seiwald and Perforce Software, Inc.
 *
 * This file is part of Jam - see jam.c for Copyright information.
 */

/*
 * filesys.h - OS specific file routines 
 *
 * 11/04/02 (seiwald) - const-ing for string literals
 */

typedef void (*scanback)( void *closure, const char *file, int found, time_t t );

void file_dirscan( const char *dir, scanback func, void *closure );
void file_archscan( const char *arch, scanback func, void *closure );

int file_time( const char *filename, time_t *time );

#ifdef unix
#define FILE_DECLARE_STOR_BUF(STOR)
#define FILE_SIMPLIFY_REL_PATH(PATH, STOR) (PATH)

#else
#define FILE_DECLARE_STOR_BUF(STOR) char STOR[260]
#define FILE_SIMPLIFY_REL_PATH(PATH, STOR) file_simplify_rel_path(PATH, STOR, sizeof(STOR))
const char *file_simplify_rel_path(const char *path, char *stor, int stor_len);

#endif
