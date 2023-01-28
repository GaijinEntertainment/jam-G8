#include "jam.h"
#include "outFilter.h"
#include "regexp.h"
#include "hash.h"

#include <string.h>
#include <ctype.h>

#if defined(WIN32) || defined(WIN64)
#include <windows.h>

CRITICAL_SECTION out_filter_critsec;
#endif

void init_exec_out_filters(ExecOutputFilter *f)
{
  memset(f, 0, sizeof(*f));
}
void destroy_exec_out_filters(ExecOutputFilter *f)
{
  int i;
  for (i = 0; i < EOF_MAX_DEST_NUM; i ++)
  {
    if (!f->destFname[i])
      break;
    free(f->destFname[i]);
    if (f->destFile[i] && f->destFile[i]!=stdout && f->destFile[i]!=stderr)
      fclose(f->destFile[i]);
  }
  for (i = 0; i < EOF_MAX_RULES_NUM; i ++)
  {
    if (!f->rules[i].re)
      break;
    free(f->rules[i].re);
    if (f->rules[i].replPattern)
      free(f->rules[i].replPattern);
    if (f->rules[i].usTab)
      hashdone(f->rules[i].usTab);
  }
  memset(f, 0, sizeof(*f));
}
int add_exec_out_filter(ExecOutputFilter *f, const char *fname, const char *re, const char *re_flg, const char *repl)
{
  int i, flags;
  for (i = 0; i < EOF_MAX_RULES_NUM; i ++)
    if (!f->rules[i].re)
    {
      int j;
      for (j = 0; j < EOF_MAX_DEST_NUM; j ++)
        if (!f->destFname[j] || stricmp(fname, f->destFname[j]) == 0)
          break;
      if (j == EOF_MAX_DEST_NUM)
      {
        printf("ERROR: no more room in destFname when adding <%s> re=<%s>\n", fname, re);
        return 0;
      }
    #if defined(WIN32)
      EnterCriticalSection(&out_filter_critsec);
		#else
		#endif
      f->rules[i].re = regcomp(re);
    #if defined(WIN32)
      LeaveCriticalSection(&out_filter_critsec);
		#else
		#endif
      if (!f->rules[i].re)
      {
        printf("ERROR: cannot compile re=<%s>\n", re);
        return 0;
      }
      if (!f->destFname[j])
        f->destFname[j] = strdup(fname);
      f->rules[i].destIdx = j;

      flags = 0;
      for ( ; *re_flg; re_flg++)
        switch (*re_flg)
        {
        case 'p': flags |= EOF_FLG_PROCEED; break;
        case 'd':
          flags |= EOF_FLG_DEPKIND;
          re_flg++;
          if (*re_flg >= '1' && *re_flg <= '8')
            flags |= *re_flg - '1';
          break;
        default: printf("ERROR: unknown regexp flag: <%c>\n", *re_flg);
        }
      f->rules[i].flags = flags;

      if (!repl)
        f->rules[i].replPattern = NULL;
      else
      {
        char *p = f->rules[i].replPattern = strdup(repl), *pt;
        int cnt = 1;

        pt = strchr(p, '$');
        while (pt)
        {
          if (pt[1] == '$')
          {
            memmove(pt, pt+1, strlen(pt));
            pt ++;
          }
          else if (pt[1] >= '0' && pt[1] <= '9')
          {
            pt[0] = '\0';
            pt += 2;
            cnt += 2;
          }
          else
          {
            printf("ERROR: bad replace pattern <%s> <%s>\n", repl, pt);
            free(f->rules[i].replPattern);
            f->rules[i].replPattern = NULL;
            return 1;
          }

          if (*pt)
            pt = strchr(pt, '$');
          else
          {
            cnt --;
            break;
          }
        }
        f->rules[i].replPatternCnt = cnt;

      }
      f->rules[i].usTab = (flags & EOF_FLG_DEPKIND) ? hashinit(sizeof(const char*), "usTab") : NULL;
      return 1;
    }
  printf("ERROR: no more room for rule re=<%s>\n", re);
  return 0;
}
void prepare_exec_out_filters(ExecOutputFilter *f)
{
  int i;
  for (i = 0; i < EOF_MAX_DEST_NUM; i ++)
  {
    if (!f->destFname[i])
      break;
    if (stricmp(f->destFname[i], "nul") == 0)
      f->destFile[i] = NULL;
    else if (stricmp(f->destFname[i], "stdout") == 0)
      f->destFile[i] = stdout;
    else if (stricmp(f->destFname[i], "stderr") == 0)
      f->destFile[i] = stderr;
    else if (!f->destFile[i])
    {
      f->destFile[i] = fopen(f->destFname[i], "wt");
      if (!f->destFile[i])
        printf("ERROR: cannot open <%s> for write, redirected to NUL\n", f->destFname[i]);
    }
  }
}

static int simplify_fname(char *dest, int dest_len, const char *src, int len)
{
  const char *s, *s_end;
  char *d, *d_end, last;
  char *ptab[16];
  int pcnt = 0;

  // replace all back slashes with normal ones, remove extra slashes
  d = dest, s = src, s_end = s+len; d_end = d+dest_len;
  *d = last = (*s == '\\') ? '/' : tolower(*s);   d++; s++;
  for (; s < s_end; s++)
    if (*s=='\\')
    {
      if (last == '.' && d>dest+1 && d[-2] == '/')
      {
        last = '/';
        d--;
      }
      else if (last != '/')
      {
        *d = last = '/';
        d++;
        if (d >= d_end)
          return -1;
      }
    }
    else if (*s == '/' && last == '.' && d>dest+1 && d[-2] == '/')
    {
      last = '/';
      d--;
    }
    else if (*s != '/' || last != '/')
    {
      *d = last = tolower(*s);
      d++;
      if (d >= d_end)
        return -1;
    }
  len = d-dest;

  d_end = dest + len;
  d = memchr(dest, '/', len);
  while (d)
  {
  repeat:
    if (d < d_end-2 && d[1] == '.' && d[2] == '.')
    {
      if (pcnt > 0)
      {
        char *p = ptab[--pcnt];
        memmove(p, d+3, d_end-d-3);
        len -= d+3-p;
        d_end -= d+3-p;
        d = p;
        if (d >= d_end)
          break;
        goto repeat;
      }
    }
    else if (pcnt < 16)
      ptab[pcnt++] = d;
    else
    {
      memmove(ptab, ptab+1, (pcnt-1)*sizeof(ptab[0]));
      ptab[pcnt-1] = d;
    }

    d = memchr(d+1, '/', d_end-d-1);
  }
  dest[len] = '\0';
  return len;
}

int exec_out_filters_process_line(ExecOutputFilter *f, const char *str)
{
  int i;
  for (i = 0; i < EOF_MAX_RULES_NUM; i ++)
  {
    if (!f->rules[i].re)
      break;
    if (regexec(f->rules[i].re, str))
    {
      FILE *fp = f->destFile[f->rules[i].destIdx];
      char *repl = f->rules[i].replPattern;
      char fnameStorage[260];
      int flg = f->rules[i].flags;

      if (flg & EOF_FLG_DEPKIND)
      {
        regexp *re = f->rules[i].re;
        int idx = (flg & EOF_FLG_DEPMASK)+1;
      	const char *str = fnameStorage, **s = &str;
      	int len = simplify_fname(fnameStorage, 260, re->startp[idx], re->endp[idx]-re->startp[idx]);
      	if (len >= 0)
      	{
          re->startp[idx] = fnameStorage;
          re->endp[idx] = fnameStorage+len;
        }
        else
          str = re->startp[idx];

      	if (hashenter(f->rules[i].usTab, (HASHDATA **)&s))
       	  *s = strdup(re->startp[idx]);
       	else
       	  fp = NULL; // skip duplicate string
      }

      if (fp)
        if (!repl)
          fprintf(fp, "%s\n", str);
        else
        {
          int repl_cnt = f->rules[i].replPatternCnt, len, idx;
          regexp *re = f->rules[i].re;
          while (repl_cnt > 1)
          {
            len = (int)strlen(repl);
            if (len)
              fputs(repl, fp);
            repl += len+1;
            idx = repl[0] - '0';
            fprintf(fp, "%.*s", (int)(re->endp[idx]-re->startp[idx]), re->startp[idx]);
            repl ++;
            repl_cnt -= 2;
          }
          if (repl_cnt)
            fprintf(fp, "%s\n", repl);
          else
            fputs("\n", fp);
        }
      if (!(flg & EOF_FLG_PROCEED))
        return 1;
    }
  }
  return 0;
}
