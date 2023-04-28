#include "jam.h"
#include "lists.h"
#include "execcmd.h"
#include "outFilter.h"
#include "variable.h"
#include <errno.h>
#ifdef unix
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#else
#include <process.h>
#include <io.h>
#ifndef __DMC__
  #include <direct.h>
#endif
#endif

#ifdef DOS386
enum { MAX_CMDLINE_LEN = 128 };
#else
#ifdef unix
long MAX_CMDLINE_LEN = 128 << 10 ;
#define FOPEN_MODE_NO_INHERIT ""
#else
enum { MAX_CMDLINE_LEN = 32768 };
#define FOPEN_MODE_NO_INHERIT "N"
#endif
#endif

/*
 * execcmd() - launch an async command execution
 */
struct ExecSlot {
  void (*completition)( void *closure, int status );
  void *closure;
  int  status;
  char *string;
  char *pending_goto;
  int useRespFile;
  ExecOutputFilter exec_out_filter;
};

static const char *delim = " \t\r\n\v";
static struct ExecSlot exec_slots[MAXJOBS] = { 0 };
static int intr = 0;
static int cmdsrunning = 0;


static void process_string_command ( struct ExecSlot *ctx, char *string, const char *pend );

#ifdef DOS386
  #define SPAWN(cmd,params,filt,one_core) _spawnl(_P_WAIT, cmd, cmd, params, NULL)
  #define SET_SIGINT(x) istat = signal(SIGINT, x)
  #define REMOVE_SIGINT() signal(SIGINT, istat)
  #define MKDIR mkdir
  #define __cdecl _cdecl
  #define strtok_r(a,b,c) strtok(a,b)

  void exec_prepare() {}
  void exec_finish() {}
  static void (_cdecl *istat)( int );
#elif defined(unix)
  #define SPAWN _unix_spawn
  #define SET_SIGINT(x) istat = signal(SIGINT, x)
  #define REMOVE_SIGINT() signal(SIGINT, istat)
  #define MKDIR(x) mkdir(x, 0777)
  #define _putenv putenv
  #define _snprintf snprintf
  #define __cdecl
  static pthread_mutex_t action_critsec;

  void exec_prepare()
  {
    MAX_CMDLINE_LEN = sysconf(_SC_ARG_MAX);
    pthread_mutexattr_t cca;
    pthread_mutexattr_init(&cca);
    pthread_mutexattr_settype(&cca, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&action_critsec, &cca);
  }
  void exec_finish()
  {
    pthread_mutex_destroy(&action_critsec);
  }
  static void EnterCriticalSection(pthread_mutex_t *cc) { pthread_mutex_lock(cc); }
  static void LeaveCriticalSection(pthread_mutex_t *cc) { pthread_mutex_unlock(cc); }
	static void Sleep(int msec) { usleep(msec ? msec*1000 : 30); }

  static void (*istat)(int);

  static intptr_t _beginthread(void *(__cdecl *start_address)(void *), unsigned stkSz, void *arg)
  {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stkSz < (256<<10))
      stkSz = 256<<10;

    pthread_attr_setstacksize(&attr, stkSz);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t id;
    int ret = pthread_create(&id, &attr, start_address, arg);
    pthread_attr_destroy(&attr);
    return ret==0 ? (intptr_t)id : -1;
  }

  int _unix_spawn(char *cmdname, char *params, ExecOutputFilter *f, int one_core)
  {
    pid_t pid, w;
    int status;

    if ((pid = fork()) == 0)
    {
      char *argv_buf[1024], *p = params, **argv = argv_buf;
      int argc = 0, argv_cap = sizeof(argv_buf)/sizeof(argv_buf[0]);
      argv[argc++] = cmdname;
      argv[argc++] = params;
      int quote_started = 0;

      while (*p)
      {
        if (!quote_started && strchr(" \t\n\r\v", *p))
        {
          *p = '\0'; p ++;
          while (*p && strchr(" \t\n\r\v", *p))
            p ++;
          if (*p)
          {
            if (argc == argv_cap)
            {
              argv_cap *= 2;
              if (argv == argv_buf)
              {
                argv = malloc(argv_cap*sizeof(char*));
                memcpy(argv, argv_buf, sizeof(argv_buf));
              }
              else
                argv = realloc(argv, argv_cap*sizeof(char*));
            }
            argv[argc++] = p;
            continue;
          }
        }
        else if (*p == '\"')
        {
          memmove(p, p+1, strlen(p));
          quote_started = !quote_started;
        }
        else if (*p == '\\')
        {
          memmove(p, p+1, strlen(p));
          switch (*p)
          {
            case 'n': *p = '\n'; break;
            case 'r': *p = '\r'; break;
            case 't': *p = '\t'; break;
          }
          p++;
        }
        else
          p++;
      }
      if (!*argv[argc-1])
        argv[argc-1] = NULL;
      else
        argv[argc] = NULL;

      execvp(argv[0], argv);
      perror("execvp");
      _exit(127);
    }
    if (pid == -1)
    {
      perror("vfork");
      exit(EXITBAD);
    }
    while( ( w = waitpid(pid, &status, 0) ) == -1 && errno == EINTR )
      ;

    if (w == -1 || w != pid)
    {
      printf("child process(es) lost w=%d pid=%d!\n", w, pid);
      perror("wait");
      exit( EXITBAD );
    }

    (void)f;
    (void)one_core;
    return status;
  }

#else
  #define SPAWN _win32_spawn
  #define SET_SIGINT(x) _win32_set_ctrl_c_handler(x)
  #define REMOVE_SIGINT() _win32_remove_ctrl_c_handler()
  #define MKDIR mkdir
  #define strtok_r(a,b,c) strtok(a,b)
  #include "win32spawnl.c"

  static void (__cdecl *_win32_onintr)(int) = NULL;
  static BOOL __cdecl interruptHandler(DWORD fdwCtrlType)
  {
    switch( fdwCtrlType )
    {
      case CTRL_C_EVENT:     _win32_onintr(SIGINT);   return TRUE;
      case CTRL_BREAK_EVENT: _win32_onintr(SIGBREAK); return TRUE;
      case CTRL_CLOSE_EVENT: _win32_onintr(SIGTERM);  return TRUE;
    }

    if (!_win32_onintr)
      return FALSE;
    _win32_onintr(SIGTERM);
    return TRUE;
  }

  void _win32_set_ctrl_c_handler(void (__cdecl *onintr)(int))
  {
    _win32_onintr = onintr;
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)interruptHandler, TRUE);
  }
  void _win32_remove_ctrl_c_handler()
  {
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)interruptHandler, FALSE);
    _win32_onintr = NULL;
  }
#endif

static int mkdir_ex ( char *fn ) {
  char *p = fn, *pslash;
  while ( *p ) {
    if ( *p == '\\' ) *p = '/';
    p ++;
  }

  pslash = strchr ( fn, '/' );
  while ( pslash ) {
    *pslash = '\0';
    MKDIR(fn);
    *pslash = '/';
    pslash = strchr ( pslash+1, '/' );
  }

  if ( MKDIR(fn) == 0 )
    return 0;
  if ( errno == EEXIST )
    return 0;
  return 27;
}

static void prepare_execcmd (struct ExecSlot *ctx) {
  ctx->pending_goto = NULL;
  ctx->useRespFile = 0;
  init_exec_out_filters(&ctx->exec_out_filter);
}
static void finish_execcmd (struct ExecSlot *ctx) {
  if (ctx->pending_goto)
    ctx->status = 27;
  destroy_exec_out_filters(&ctx->exec_out_filter);
}
static int copy_file ( const char *src, const char *dest ) {
  //printf ( "copy %s to %s\n", src, dest );
  FILE *fp_in, *fp_out;
  char buf[4096];

  if ( !src || !dest )
    return 13;
  fp_in = fopen ( src, "rb" );
  if ( !fp_in )
    return 13;
  fp_out = fopen ( dest, "wb" FOPEN_MODE_NO_INHERIT);
  if ( !fp_out ) {
    fclose ( fp_in );
    return 13;
  }

  while ( !feof ( fp_in )) {
    int cnt = (int)fread ( buf, 1, 4096, fp_in );
    if (fwrite ( buf, 1, cnt, fp_out ) != cnt)
      return 13;
  }
  fclose ( fp_out );
  fclose ( fp_in );
  return 0;
}

static void process_command (struct ExecSlot *ctx, char *cmd, char *params ) {
  char *brkt;
  //printf ( "cmd=<%s> params=<%s>\n", cmd, params );
  (void)(brkt);

  if ( ctx->pending_goto && cmd[0] != ':' )
    return;

  // replace %errorlevel%
  if ( params ) {
    char *p, *pp = params;
    fflush ( stdout );
    do {
      p = strstr ( pp, "%errorlevel%" );
      if ( !p )
        p = strstr ( pp, "%ERRORLEVEL%" );
      if ( p ) {
        sprintf ( p, "%d", ctx->status );
        pp = p+strlen(p);
        if ( pp < p+12 )
          memmove ( pp, p+12, strlen(p+12)+1);
      }
    } while ( p );
  } else
    params = "";

  // process command
  if ( stricmp ( cmd, "echo" ) == 0 || stricmp ( cmd, "echo_cvt" ) == 0 ) {
    char *p = strchr ( params, '>' );

    if ( stricmp ( cmd, "echo_cvt" ) == 0 ) {
      char *p2 = params;
      while ( *p2 && p2 < p ) {
        if ( *p2 == '/' ) *p2 = '\\';
        p2 ++;
      }
    }


    if ( !p )
      fprintf(stdout, "%s\n", params);
    else {
      char *fname = p+1;
      FILE *fp;

      *p = '\0';
      if ( *fname == '>' )
        fname ++;
      fname = strtok_r(fname, delim, &brkt);
      fp = fopen ( fname, p[1]=='>' ? "at" FOPEN_MODE_NO_INHERIT : "wt" FOPEN_MODE_NO_INHERIT);
      if ( fp ) {
        fputs ( params, fp );
        fputc ( '\n', fp );
        fclose ( fp );
      } else {
        printf ( "incorrect file: <%s>\n", fname );
        ctx->status = 1;
      }
    }
  } else if ( stricmp ( cmd, "mkdir" ) == 0 ) {
    ctx->status = mkdir_ex ( params );
  } else if ( stricmp ( cmd, "copyfile" ) == 0 ) {
    char *token = strtok_r(params, " \t\n\r\v", &brkt);
    ctx->status = copy_file ( token, strtok_r(NULL, " \t\n\r\v", &brkt));
  } else if ( stricmp ( cmd, "del" ) == 0 ) {
    ctx->status = unlink ( params );
  } else if ( stricmp ( cmd, "rem" ) == 0 || cmd[0] == '#' ) {
    if (stricmp(cmd, "#respfile") == 0)
      ctx->useRespFile = 1;
    else if (stricmp(cmd, "#no-respfile") == 0)
      ctx->useRespFile = 0;
    // remark, do nothing
  } else if ( stricmp ( cmd, "set" ) == 0 ) {
    char *p = strchr ( params, '=' ), *token;
    if ( !p ) return;
    *p = '\0'; p ++;

    token = strtok_r(params, delim, &brkt);
    if ( !token ) return;

    if ( stricmp ( token, "errorlevel" ) == 0 ) {
      token = strtok_r(p, delim, &brkt);
      if ( !token ) return;
      ctx->status = atoi ( token );
    } else {
      char buf[2048];
      _snprintf ( buf, 2048, "%s=%s", token, p );
      buf[2047] = '\0';
      _putenv ( buf );
    }
  } else if ( cmd[0] == ':' ) {
    char *label = cmd[1] ? &cmd[1] : params;
    if ( !ctx->pending_goto ) return;
    if ( stricmp ( ctx->pending_goto, label ) == 0 )
      ctx->pending_goto = NULL;
  } else if ( stricmp ( cmd, "goto" ) == 0 ) {
    ctx->pending_goto = params;
  } else if ( stricmp ( cmd, "if" ) == 0 ) {
    char *token = strtok_r(params, " \t\n\r\v=", &brkt);
    int neg = 0, res;

    if ( !token ) goto err_if;
    if ( stricmp ( token, "not" ) == 0 ) {
      neg = 1;
      token = strtok_r(NULL, delim, &brkt);
    }

    if ( !token ) goto err_if;
    if ( stricmp ( token, "exist" ) == 0 ) {
      token = strtok_r(NULL, delim, &brkt);
      if ( !token ) goto err_if;
      res = access ( token, 0 ) == 0;
    } else if ( stricmp ( token, "errorlevel" ) == 0 ) {
      token = strtok_r(NULL, delim, &brkt);
      if ( !token ) goto err_if;
      res = ctx->status >= atoi(token);
    } else
      goto err_if;

    if ( neg ) res = !res;
    token = strtok_r(NULL, "", &brkt);
    if ( !token ) goto err_if;
    if ( token[0] == '(' ) {
      printf ( "multiline if not supported\n" );
      goto err_if;
    }

    if ( res ) {
      while ( *token && strchr ( delim, *token ))
        token ++;
      process_string_command(ctx, token, token+strlen(token)-1);
      return;
    } else
      return;


  err_if:
    printf ( "unrecognized command IF (last token=%s)\n", token );
    ctx->status = 26;
  } else if ( stricmp ( cmd, "set_filter" ) == 0 ) {
    char *repl = NULL, *re_attr = "";
    int bad_re = 1;

    cmd = strtok_r(params, " \t\n\r\v", &brkt);
    params = strtok_r(NULL, "", &brkt);
    if (params[0] != '/')
    {
    bad_regexp:
      printf("bad regexp format regexp=%s\n", params);
      return;
    }
    params++;
    repl = params;
    while (*repl)
    {
      if (*repl == '/')
      {
        *repl = '\0';
        repl++;
        bad_re = 0;
        break;
      }
      if (*repl == '\\')
      {
        repl ++;
        if (!*repl)
          break;
      }
      repl ++;
    }
    if (bad_re)
      goto bad_regexp;
    if (repl)
    {
      re_attr = repl;
      while (*repl && !strchr(" \t\n\r\v", *repl))
        repl++;
      if (*repl)
      {
        *repl = '\0';
        repl++;
        while (*repl && strchr(" \t\n\r\v", *repl))
          repl ++;
      }
    }

    add_exec_out_filter(&ctx->exec_out_filter, cmd, params, re_attr, repl && *repl ? repl : NULL);
  } else if ( stricmp ( cmd, "clr_filters" ) == 0 ) {
    destroy_exec_out_filters(&ctx->exec_out_filter);
  } else if ( stricmp ( cmd, "enter_critsec" ) == 0 ) {
#if !defined(DOS386)
    if (globs.jobs > 1)
      EnterCriticalSection(&action_critsec);
#endif
  } else if ( stricmp ( cmd, "leave_critsec" ) == 0 ) {
#if !defined(DOS386)
    if (globs.jobs > 1)
      LeaveCriticalSection(&action_critsec);
#endif
  } else if (stricmp(cmd, "call") == 0 || stricmp(cmd, "call_filtered") == 0 ||
             stricmp(cmd, "call_1core") == 0 || stricmp(cmd, "call_1core_filtered") == 0) {
    int filtered = stricmp ( cmd, "call_filtered" ) == 0 || stricmp(cmd, "call_1core_filtered") == 0;
    int use1core = stricmp ( cmd, "call_1core" ) == 0 || stricmp(cmd, "call_1core_filtered") == 0;
    char *resp_start = params;

    // run program command
    if ( !params || !params[0] ) {
	    printf ( "call: parameters missing\n" );
	    ctx->status = 26;
      return;
    }
    cmd = strtok_r(params, " \t\n\r\v", &brkt);
    params = strtok_r(NULL, "", &brkt);
    resp_start = strstr(params, "@@@");
    if (resp_start)
    {
      memset(resp_start, ' ', 3);
      resp_start += 4; // +1 to skip trailing ' '
    }
    else
      resp_start = params;

#ifndef unix
    {
      char *p;
      for (p = cmd; *p; p++)
        if (*p == '/')
          *p = '\\';
    }
#endif

    if ( strlen ( cmd ) + strlen ( params ) + 8 < (ctx->useRespFile ? 1024 : MAX_CMDLINE_LEN) ) {
      // direct exec
      ctx->status = SPAWN(cmd, params, filtered ? &ctx->exec_out_filter : NULL, use1core);
    }
    else {
      // exec with response file
      char temp[512];
      char param_str[512];
      FILE *fp;
      temp[0] = '@';

#ifndef unix
      char *ptr = resp_start;
      for (; *ptr; ptr++)
        if (*ptr == ' ')
          *ptr = '\n';
#endif

#ifdef unix
      strcpy(temp+1, "/tmp/jam_respXXXXXX");
      int tmph = mkstemp(temp+1);
      fp = fdopen (tmph, "wt");
#else
      _snprintf(temp+1, sizeof(temp)-1, "%s\\jam_respXXXXXX", getenv("TMP"));
      fp = fopen (mktemp(temp+1), "wtN");
#endif
      if (!fp)
      {
        unlink(temp+1);
        printf("err: failed to write to newly created response-file %s\n", temp+1);
        ctx->status = 26;
        return;
      }
      fputs ( resp_start, fp );
      fclose ( fp );

      if (resp_start > params)
        _snprintf(param_str, sizeof(param_str)-1, "%.*s %s", (int)(resp_start-params), params, temp);
      else
        strcpy(param_str, temp);

      ctx->status = SPAWN(cmd, param_str, filtered ? &ctx->exec_out_filter : NULL, use1core);

      unlink(temp+1);
    }
  } else {
    printf ( "unknown command: %s %s\n(tip: use 'call' command to exec program)\n", cmd, params ? params : "" );
    ctx->status = 26;
  }
}

static void process_string_command (struct ExecSlot *ctx,  char *string, const char *p) {
  char *command = string;

  while ( string <= p && !strchr ( " \r\n\t", *string ))
    string ++;
  if ( string <= p ) {
    *string = '\0';
    string ++;

    while ( string < p && strchr ( "\r\n\t", *string ))
      string ++;

    process_command(ctx, command, string);
  } else if ( *command )
    process_command(ctx, command, NULL);
}

static int preprocess_string_backslash(char *string) {
  char *p = strstr ( string, "#\\(" ), *pe;
  size_t len = strlen ( string )+1;

  while ( p ) {
    pe = strstr ( p+3, ")\\#" );
    if ( !pe )
      return 0;
    memmove ( p, p+3, pe-p-3 );
    memmove ( pe-3, pe+3, len - (pe+3-string));
    len -= 6;

    while ( p < pe-3 ) {
      if ( *p == '/' ) *p = '\\';
      p ++;
    }
    p = strstr ( pe-3, "#\\(" );
  }
  return 1;
}

static int preprocess_string_slash(char *string) {
  char *p = strstr ( string, "#/(" ), *pe;
  size_t len = strlen ( string )+1;

  while ( p ) {
    pe = strstr ( p+3, ")/#" );
    if ( !pe )
      return 0;
    memmove ( p, p+3, pe-p-3 );
    memmove ( pe-3, pe+3, len - (pe+3-string));
    len -= 6;

    while ( p < pe-3 ) {
      if ( *p == '\\' ) *p = '/';
      p ++;
    }
    p = strstr ( pe-3, "#/(" );
  }
  return 1;
}

void __cdecl onintr( int disp )
{
  intr++;
  if (intr == 1)
    printf("...interrupted\n");
  else if (intr == 2)
    printf("...interrupted - be patient while processes finish working\n");
}

#if defined(unix)
static void *__cdecl exec_job_thread(void *p)
#else
static void __cdecl exec_job_thread(void *p)
#endif
{
  struct ExecSlot *ctx = (struct ExecSlot*)p;
  char *string = ctx->string;

  if (intr)
    goto end;

  // process command string
  prepare_execcmd(ctx);
  preprocess_string_slash(string);
  preprocess_string_backslash(string);
  while ( *string ) {
    char *pend, *p;

    // skip leading whitespace
    while ( *string && strchr ( " \r\n\t", *string ))
      string ++;

    pend = strchr ( string, '\n' );
    if ( pend )
      pend ++;
    else
      pend = string+strlen(string);

    // skip trailing whitespace
    p = pend - 1;
    while ( p > string && strchr ( " \r\n\t", *p )) {
      *p = '\0';
      p --;
    }

    process_string_command (ctx, string, p);

    string = pend;
  }
  finish_execcmd(ctx);

end:
  if ( intr )
    ctx->status = 255; //interrupted
  free(ctx->string);
  ctx->string = NULL;
#if defined(unix)
  return NULL;
#endif
}

static const char *check_async_label(char *str)
{
  while (*str && strchr(" \n\r\t", *str))
    str ++;
  return strncmp(str, "#async", 6)==0 ? str+6 : NULL;
}
void execcmd( char *string, void (*func)( void *closure, int status ), void *closure,
              LIST *shell )
{
  int slot;
  const char *req_async = check_async_label(string);

  while (cmdsrunning >= globs.jobs || cmdsrunning >= MAXJOBS)
    if( !execwait() )
  		break;

  // Find a slot in the running commands table for this one
  for( slot = 0; slot < MAXJOBS; slot++ )
    if( !exec_slots[slot].completition )
      break;

  if( slot >= MAXJOBS )
  {
    printf( "no slots for child!\n" );
    exit( EXITBAD );
  }

  exec_slots[slot].completition = func;
  exec_slots[slot].closure = closure;
  exec_slots[slot].status = 0;
  exec_slots[slot].string = intr ? NULL : strdup(req_async ? req_async : string);
  string[0] = '\0';

  // Catch interrupts whenever commands are running
  if (!cmdsrunning)
    SET_SIGINT(onintr);
  cmdsrunning++;

  if (exec_slots[slot].string)
#if defined(DOS386)
    exec_job_thread(&exec_slots[slot]);
#else
    if (!req_async || globs.jobs<2 ||
        _beginthread(exec_job_thread, 64<<10, &exec_slots[slot]) == -1)
      exec_job_thread(&exec_slots[slot]);
#endif
}

/*
 * execwait() - wait and drive at most one execution completion
 */
int execwait()
{
  void (*compl)( void *, int );
  int i, rstat;

  if( !cmdsrunning )
    return 0;

  for ( i = 0; i < MAXJOBS; i ++ )
    if ( exec_slots[i].completition && !exec_slots[i].string )
      break;

#if !defined(DOS386)
	if( i == MAXJOBS )
	{
	  Sleep(10);
	  return 1;
	}
#endif

	cmdsrunning --;
  if (!cmdsrunning)
    REMOVE_SIGINT();

  if (intr)
    rstat = EXEC_CMD_INTR;
  else if( exec_slots[i].status != 0 )
    rstat = EXEC_CMD_FAIL;
  else
    rstat = EXEC_CMD_OK;

  // clear slot and call completition routine
  compl = exec_slots[i].completition;
  exec_slots[i].completition = NULL;

  compl ( exec_slots[i].closure, rstat );

  return 1;
}
