/*
** 2014-06-13
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This SQLite extension implements SQL functions readfile() and
** writefile(), and eponymous virtual type "fsdir".
**
** WRITEFILE(FILE, DATA [, MODE [, MTIME]]):
**
**   If neither of the optional arguments is present, then this UDF
**   function writes blob DATA to file FILE. If successful, the number
**   of bytes written is returned. If an error occurs, NULL is returned.
**
**   If the first option argument - MODE - is present, then it must
**   be passed an integer value that corresponds to a POSIX mode
**   value (file type + permissions, as returned in the stat.st_mode
**   field by the stat() system call). Three types of files may
**   be written/created:
**
**     regular files:  (mode & 0170000)==0100000
**     symbolic links: (mode & 0170000)==0120000
**     directories:    (mode & 0170000)==0040000
**
**   For a directory, the DATA is ignored. For a symbolic link, it is
**   interpreted as text and used as the target of the link. For a
**   regular file, it is interpreted as a blob and written into the
**   named file. Regardless of the type of file, its permissions are
**   set to (mode & 0777) before returning.
**
**   If the optional MTIME argument is present, then it is interpreted
**   as an integer - the number of seconds since the unix epoch. The
**   modification-time of the target file is set to this value before
**   returning.
**
**   If three or more arguments are passed to this function and an
**   error is encountered, an exception is raised.
**
** READFILE(FILE):
**
**   Read and return the contents of file FILE (type blob) from disk.
**
** FSDIR:
**
**   Used as follows:
**
**     SELECT * FROM fsdir($path [, $dir]);
**
**   Parameter $path is an absolute or relative pathname. If the file that it
**   refers to does not exist, it is an error. If the path refers to a regular
**   file or symbolic link, it returns a single row. Or, if the path refers
**   to a directory, it returns one row for the directory, and one row for each
**   file within the hierarchy rooted at $path.
**
**   Each row has the following columns:
**
**     name:  Path to file or directory (text value).
**     mode:  Value of stat.st_mode for directory entry (an integer).
**     mtime: Value of stat.st_mtime for directory entry (an integer).
**     data:  For a regular file, a blob containing the file data. For a
**            symlink, a text value containing the text of the link. For a
**            directory, NULL.
**
**   If a non-NULL value is specified for the optional $dir parameter and
**   $path is a relative path, then $path is interpreted relative to $dir. 
**   And the paths returned in the "name" column of the table are also 
**   relative to directory $dir.
*/
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#if !defined(_WIN32) && !defined(WIN32)
#  include <unistd.h>
#  include <dirent.h>
#  include <utime.h>
#  include <sys/time.h>
#else
#  include "windows.h"
#  include <io.h>
#  include <direct.h>
#  define dirent DIRENT
#  ifndef chmod
#    define chmod _chmod
#  endif
#  ifndef stat
#    define stat _stat
#  endif
#  define mkdir(path,mode) _mkdir(path)
#  define lstat(path,buf) stat(path,buf)
#endif
#include <time.h>
#include <errno.h>


/*
** Structure of the fsdir() table-valued function
*/
                 /*    0    1    2     3    4           5             */
#define FSDIR_SCHEMA "(name,mode,mtime,data,path HIDDEN,dir HIDDEN)"
#define FSDIR_COLUMN_NAME     0     /* Name of the file */
#define FSDIR_COLUMN_MODE     1     /* Access mode */
#define FSDIR_COLUMN_MTIME    2     /* Last modification time */
#define FSDIR_COLUMN_DATA     3     /* File content */
#define FSDIR_COLUMN_PATH     4     /* Path to top of search */
#define FSDIR_COLUMN_DIR      5     /* Path is relative to this directory */

int isJSON(const char *str)
{
    char *dot = strrchr(str, '.');

    if (NULL == dot) return 0;
    return strcmp(dot, ".json") == 0;
}

/*
** Set the result stored by context ctx to a blob containing the 
** contents of file zName.  Or, leave the result unchanged (NULL)
** if the file does not exist or is unreadable.
**
** If the file exceeds the SQLite blob size limit, through an
** SQLITE_TOOBIG error.
**
** Throw an SQLITE_IOERR if there are difficulties pulling the file
** off of disk.
*/
static void readFileContents(sqlite3_context *ctx, const char *zName){
  FILE *in;
  sqlite3_int64 nIn;
  void *pBuf;
  sqlite3 *db;
  int mxBlob;

  in = fopen(zName, "rb");
  if( in==0 ){
    /* File does not exist or is unreadable. Leave the result set to NULL. */
    return;
  }
  fseek(in, 0, SEEK_END);
  nIn = ftell(in);
  rewind(in);
  db = sqlite3_context_db_handle(ctx);
  mxBlob = sqlite3_limit(db, SQLITE_LIMIT_LENGTH, -1);
  if( nIn>mxBlob ){
    sqlite3_result_error_code(ctx, SQLITE_TOOBIG);
    fclose(in);
    return;
  }
  if(nIn == 0){
    fclose(in);
    
    sqlite3_result_text(ctx, "[]", -1, SQLITE_TRANSIENT);
    return;
  }
  pBuf = sqlite3_malloc64( nIn ? nIn : 1 );
  if( pBuf==0 ){
    sqlite3_result_error_nomem(ctx);
    fclose(in);
    return;
  }
  if( nIn==(sqlite3_int64)fread(pBuf, 1, (size_t)nIn, in) ){
    sqlite3_result_blob64(ctx, pBuf, nIn, sqlite3_free);
  }else{
    sqlite3_result_error_code(ctx, SQLITE_IOERR);
    sqlite3_free(pBuf);
  }
  fclose(in);
}

static void cd(sqlite3_context *ctx, const char *zName){
  chdir(zName);
}

/*
** Implementation of the "readfile(X)" SQL function.  The entire content
** of the file named X is read and returned as a BLOB.  NULL is returned
** if the file does not exist or is unreadable.
*/
static void readfileFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  const char *zName;
  (void)(argc);  /* Unused parameter */
  zName = (const char*)sqlite3_value_text(argv[0]);
  if( zName==0 ) return;
  readFileContents(context, zName);
}

static void cdFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  const char *zName;
  (void)(argc);  /* Unused parameter */
  zName = (const char*)sqlite3_value_text(argv[0]);
  if( zName==0 ) return;
  cd(context, zName);
}

/*
** Set the error message contained in context ctx to the results of
** vprintf(zFmt, ...).
*/
static void ctxErrorMsg(sqlite3_context *ctx, const char *zFmt, ...){
  char *zMsg = 0;
  va_list ap;
  va_start(ap, zFmt);
  zMsg = sqlite3_vmprintf(zFmt, ap);
  sqlite3_result_error(ctx, zMsg, -1);
  sqlite3_free(zMsg);
  va_end(ap);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_fileio_init(
  sqlite3 *db, 
  char **pzErrMsg, 
  const sqlite3_api_routines *pApi
){
  int rc = SQLITE_OK;
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;  /* Unused parameter */
  rc = sqlite3_create_function(db, "readfile", 1, SQLITE_UTF8, 0,
                               readfileFunc, 0, 0);
  rc = sqlite3_create_function(db, "cd", 1, SQLITE_UTF8, 0,
                               cdFunc, 0, 0);
  return rc;
}
