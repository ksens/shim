#include <time.h>
#include <unistd.h>
#define TOK_BUF 128

int do_pam_login (char *, char *, char *);
unsigned long authtoken();
/* List of authenticated user tokens */
typedef struct
{
  unsigned long val;       // The token
  time_t time;             // last access time (used to timeout tokens)
  uid_t uid;
  void *next;
} token_list;
token_list * addtoken(token_list *, unsigned long, uid_t uid);
token_list * removetoken(token_list *, unsigned long);
uid_t username2uid(char *username);
