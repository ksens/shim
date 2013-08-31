#include <time.h>
#define TOK_BUF 128
int do_pam_login (char *, char *, char *);
unsigned long authtoken();
/* List of authenticated user tokens */
typedef struct
{
  unsigned long val;       // The token
  time_t time;             // last access time (used to timeout tokens)
  void *next;
} token_list;
token_list * addtoken(token_list *, unsigned long);
token_list * removetoken(token_list *, unsigned long);
