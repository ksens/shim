/*
 * Portions of this file are adapted from the Pam.cpp source file
 * copyrighted by RStudio, Inc. licensed under the AGPL 3. The
 * original copyright statement follows.
 *
 * Copyright (C) 2009-12 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 * Additional portions based on Per-Aake Larson's Dynamic Hashing algorithms.
 * BIT 18 (1978).  and public domain example by oz@nexus.yorku.ca.
 */

#include <security/pam_appl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "pam.h"
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

/* A conversation function for PAM to non-interactively supply username
 * and password.
 */
int
conv (int num_msg,
      const struct pam_message **msg,
      struct pam_response **resp, void *appdata_ptr)
{
  int i;
/* Note: The PAM caller will free response. */
  *resp =
    (struct pam_response *) malloc (sizeof (struct pam_response) * num_msg);
  if (!*resp)
    return PAM_BUF_ERR;
  memset (*resp, 0, sizeof (struct pam_response) * num_msg);
  for (i = 0; i < num_msg; i++)
    {
      const struct pam_message *input = msg[i];
      switch (input->msg_style)
        {
        case PAM_PROMPT_ECHO_OFF:
          {
            resp[i]->resp_retcode = 0;
/* Note: The PAM caller will free resp[i]->resp here or below. */
            resp[i]->resp = strdup ((char *) appdata_ptr);
            break;
          }
        case PAM_TEXT_INFO:
          {
            resp[i]->resp_retcode = 0;
            char *respBuf = malloc (1);
            respBuf[0] = '\0';
            resp[i]->resp = respBuf;
            break;
          }
        case PAM_PROMPT_ECHO_ON:
        case PAM_ERROR_MSG:
        default:
          return PAM_CONV_ERR;
        }
    }
  return PAM_SUCCESS;
}

/* service: PAM service name (for example, "login")
 * username, password: just what they say
 * returns 0 with successul authentication, non-zero otherwise.
 */
int
do_pam_login (char *service, char *username, char *password)
{
  int k;
  struct pam_conv pamc;
  pam_handle_t *pamh;
  pamc.conv = conv;
  pamc.appdata_ptr = (void *) password;
  k = pam_start (service, username, &pamc, &pamh);
  if (k != PAM_SUCCESS)
    goto end;

  k = pam_authenticate (pamh, 0);
  if (k != PAM_SUCCESS)
    goto end;
end:
  pam_end (pamh, k | PAM_SILENT);
/* just in case PAM_SUCCESS someday becomes nonzero */
  if (k == PAM_SUCCESS)
    k = 0;
  return k;
}

/* A simple token generator using the basic sdbm hash function.  We use it to
 * generate login token ids.  This token generator returns 0 if something went
 * wrong.
 *
 * based on Per-Aake Larson's Dynamic Hashing algorithms. BIT 18 (1978).
 * and public domain example by oz@nexus.yorku.ca
 * status: public domain. keep it that way.
 */
unsigned long
authtoken()
{
  int j;
  char *p;
  char buf[TOK_BUF];
  unsigned long ans = 0;
/* disallow too short tokens */
  while(ans < 10000)
  {
    FILE *f = fopen( "/dev/urandom", "r");
    if(!f) return 0;
    fread(buf, 1, TOK_BUF, f);
    fclose(f);
    p = buf;
    while((j=*p++))
      ans = j + (ans << 6) + (ans << 16) - ans;
  }
  return ans;
}

/* addtoken
 * Add a new authtoken to the list. If there is a conflict, return NULL and
 * don't add the new token. Otherwise return the new head (and modify head)
 * that points to the new head of the list. Aquire the big lock before calling
 * this.
 */
token_list *
addtoken(token_list *head, unsigned long val, uid_t uid)
{
  token_list *new;
  token_list *t = head;
/* Scan the list for collision. Also eject timed out tokens from the list. */
  while(t)
  {
    if(t->val == val) return NULL;
// XXX check time out (todo)
    t = (token_list *)t->next;
  }
  new = (token_list *)malloc(sizeof(token_list));
  new->val = val;
  new->uid = uid;
  new->time = time(NULL);
  new->next = (void *)head;
  head = new;
  return head;
}

/* removetoken
 * remove a token from the list, deallocating the token entry.
 * Return the head of the list (which may change).
 */
token_list *
removetoken(token_list *item, unsigned long val)
{
  if (item == NULL)
    return NULL;
  if (item->val == val)
  {
    token_list * nextitem;
    nextitem = item->next;
    free(item);
    return nextitem;
  }
  item->next = removetoken(item->next, val);
  return item;
}

/* Look up a uid from a user name string. Return an
 * invalid uid_t if not found.
 */
uid_t
username2uid(char *username)
{
  struct passwd pwd;
  struct passwd *result;
  char *buf;
  size_t bufsize;

  bufsize = sysconf (_SC_GETPW_R_SIZE_MAX);
  if (bufsize == -1)            /* Value was indeterminate */
    bufsize = 16384;            /* Should be more than enough */

  buf = malloc (bufsize);
  if (buf == NULL)
    {
      return -1;
    }
  getpwnam_r (username, &pwd, buf, bufsize, &result);
  if (result == NULL)
    {
      free(buf);
      return -1;
    }

  free(buf);
  return pwd.pw_uid;
}
