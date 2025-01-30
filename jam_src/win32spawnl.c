#include <windows.h>

extern CRITICAL_SECTION out_filter_critsec;
static CRITICAL_SECTION action_critsec;
static int stdout_bin = -1;

void exec_prepare()
{
  InitializeCriticalSection(&out_filter_critsec);
  InitializeCriticalSection(&action_critsec);
  stdout_bin = _dup(fileno(stdout));
  _setmode(stdout_bin, _O_BINARY);
}
void exec_finish()
{
  DeleteCriticalSection(&action_critsec);
  DeleteCriticalSection(&out_filter_critsec);
  if (stdout_bin>=0)
    close(stdout_bin);
  stdout_bin = -1;
}

#ifdef __DMC__
typedef int intptr_t;
#endif

intptr_t _win32_spawn(const char *cmdname, const char *params, ExecOutputFilter *f, int one_core)
{
  SECURITY_ATTRIBUTES sa;
  PROCESS_INFORMATION pi;
  STARTUPINFO si;
  HANDLE hProc = GetCurrentProcess();
  HANDLE output_read, output_write;
  HANDLE stdout_write, stderr_write;
  DWORD ret_code;
  char *cmdline;
  int flags = CREATE_NEW_CONSOLE;

  sa.nLength = sizeof (SECURITY_ATTRIBUTES);
  sa.lpSecurityDescriptor = NULL;
  sa.bInheritHandle = TRUE;

  if (!CreatePipe (&output_read, &output_write, &sa, 0))
    return -1;

  // Disable parent process pipe handle inheritance
  // https://learn.microsoft.com/en-us/windows/win32/procthread/creating-a-child-process-with-redirected-input-and-output
  if (!SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0))
    goto err_exit_0;

  if (!DuplicateHandle(hProc, output_write, hProc, &stdout_write, 0, TRUE, DUPLICATE_SAME_ACCESS))
    goto err_exit_0;
  if (!DuplicateHandle (hProc, output_write, hProc, &stderr_write, 0, TRUE, DUPLICATE_SAME_ACCESS))
    goto err_exit_1;

  ZeroMemory (&si, sizeof (STARTUPINFO));
  si.cb = sizeof (STARTUPINFO);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = stdout_write;
  si.hStdError = stderr_write;
  si.wShowWindow = SW_HIDE;
  if (one_core)
    flags |= CREATE_SUSPENDED;

  cmdline = malloc(strlen(cmdname)+strlen(params)+2);
  sprintf(cmdline, "%s %s", cmdname, params);

  if (!CreateProcessA(cmdname, cmdline, NULL, NULL, TRUE, flags, NULL, NULL, &si, &pi))
    goto err_exit_2;
  free(cmdline);
  if (one_core)
  {
    SetProcessAffinityMask(pi.hProcess, 1);
    ResumeThread(pi.hThread);
  }

  if (f)
    prepare_exec_out_filters(f);
  
  CloseHandle(stdout_write);
  CloseHandle(stderr_write);
  CloseHandle(output_write);

  if (f)
  {
    enum { BUF_SZ = 16384 };
    char *buf = malloc(BUF_SZ), *p = buf, *pe = p;
    DWORD bytes_read;
    for (;;)
    {
      if (!ReadFile (output_read, pe, buf+BUF_SZ-8-pe, &bytes_read, NULL) || !bytes_read)
        break; // pipe done - normal exit path.

      EnterCriticalSection(&out_filter_critsec);

      pe += bytes_read;
      *pe = '\0';
      while (pe > p)
      {
        char *pt = memchr(p, '\n', pe-p);
        if (!pt && pe-p > BUF_SZ/2)
          pt = p+BUF_SZ/2;

        if (pt)
        {
          *pt = '\0';
          if (pt > p && pt[-1] == '\r')
            pt[-1] = '\0';

          if (!exec_out_filters_process_line(f, p))
            printf("%s\n", p);

          p = pt+1;
          if (p < pe && *p == '\r')
          {
            *p = '\0';
            p ++;
          }
          continue;
        }

        if (p > buf)
        {
          int rest = pe-p;
          memmove(buf, p, rest);
          p = buf;
          pe = p+rest;
          *pe = '\0';
        }
        break;
      }
      if (pe == p)
        pe = p = buf;
      LeaveCriticalSection(&out_filter_critsec);
    }
    if (pe > p)
    {
      EnterCriticalSection(&out_filter_critsec);
      pe[0] = '\0';
      if (!exec_out_filters_process_line(f, p))
        printf("%s\n", p);
      LeaveCriticalSection(&out_filter_critsec);
    }
    free(buf);
  }
  else
    for (;;)
    {
      char buf[1024];
      DWORD bytes_read;

      if (!ReadFile (output_read, buf, 1024, &bytes_read, NULL) || !bytes_read)
        break; // pipe done - normal exit path.

      write(stdout_bin, buf, bytes_read);
    }

  CloseHandle(output_read);

  WaitForSingleObject(pi.hProcess, INFINITE);
  GetExitCodeProcess(pi.hProcess, &ret_code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  return ret_code;

err_exit_2:
  printf("ERROR: failed exec <%s> <%s>\n", cmdname, params);
  free(cmdline);
  CloseHandle(stderr_write);
err_exit_1:
  CloseHandle(stdout_write);
err_exit_0:
  CloseHandle(output_read);
  CloseHandle(output_write);
  return -1;
}
