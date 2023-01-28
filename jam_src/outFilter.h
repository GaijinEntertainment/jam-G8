#include <stdio.h>

struct regexp;
struct hash;

enum
{
  EOF_MAX_DEST_NUM = 8,
  EOF_MAX_RULES_NUM = 32,

  EOF_FLG_DEPMASK = 0x0007, // mask for DEPKIND flag, maps 0..7 to match1..match8 to treat as filename
  EOF_FLG_DEPKIND = 0x0008, // 'dN', treat $N subgroup as dep filepath, simplify filename and output only distinct matches
  EOF_FLG_PROCEED = 0x0010, // 'p', continue checkign other regexp even after match
};

typedef struct ExecOutputFilter
{
  char *destFname[EOF_MAX_DEST_NUM];
  FILE *destFile[EOF_MAX_DEST_NUM];
  struct Rule
  {
    struct regexp *re;
    short flags;
    char destIdx;
    char replPatternCnt;
    char *replPattern;
    struct hash *usTab;
  } rules[EOF_MAX_RULES_NUM];
} ExecOutputFilter;

void init_exec_out_filters(ExecOutputFilter *f);
void destroy_exec_out_filters(ExecOutputFilter *f);
int  add_exec_out_filter(ExecOutputFilter *f, const char *fname, const char *re, const char *re_flags, const char *repl);
void prepare_exec_out_filters(ExecOutputFilter *f);

int  exec_out_filters_process_line(ExecOutputFilter *f, const char *str);
