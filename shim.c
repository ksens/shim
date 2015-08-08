#define _GNU_SOURCE
#include <stdio.h>
#include <limits.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <syslog.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>
#include <omp.h>
#include <signal.h>
#include "mongoose.h"
#include "pam.h"
#include <pwd.h>

#define DEFAULT_MAX_SESSIONS 50  // Maximum number of concurrent http sessions
#define MAX_VARLEN 4096          // Static buffer length for http query params
#define LCSV_MAX 16384
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define MAX_RETURN_BYTES INT_MAX

#ifndef DEFAULT_HTTP_PORT
#define DEFAULT_HTTP_PORT "8080,8083s"
#endif

#define DEFAULT_SAVE_INSTANCE_ID 0 // default instance that does the saving
#define DEFAULT_TMPDIR "/tmp/"  // Temporary location for I/O buffers
#define PIDFILE "/var/run/shim.pid"

#define WEEK 604800             // One week in seconds
#define DEFAULT_TIMEOUT 60      // Timeout before a session is declared
                                // orphaned and available to reap (seconds)

// Minimalist SciDB client API from client.cpp -------------------------------
void *scidbconnect (const char *host, int port);
void scidbdisconnect (void *con);
unsigned long long executeQuery (void *con, char *query, int afl, char *err);
void completeQuery (unsigned long long id, void *con, char *err);
void prepare_query (void *, void *, char *, int, char *);
// A support structure for prepare_query/execute_prepared_query
struct prep
{
  unsigned long long queryid;
  void *queryresult;
};
unsigned long long execute_prepared_query (void *, char *, struct prep *, int,
                                           char *);
// End of mimimalist SciDB client API -----------------------------------------

/* A session consists of client I/O buffers, and an optional SciDB query ID. */
typedef enum
{
  SESSION_UNAVAILABLE,
  SESSION_AVAILABLE
} available_t;
typedef struct
{
  omp_lock_t lock;
  int sessionid;                // session identifier
  unsigned long long queryid;   // SciDB query identifier
  int pd;                       // output buffer file descrptor
  FILE *pf;                     //   and FILE pointer
  int stream;                   // non-zero if output streaming enabled
  int save;                     // non-zero if output is to be saved/streamed
  int compression;              // gzip compression level for stream
  char *ibuf;                   // input buffer name
  char *obuf;                   // output (file) buffer name
  char *opipe;                  // output pipe name
  void *con;                    // SciDB context
  time_t time;                  // Time value to help decide on orphan sessions
  available_t available;        // 1 -> available, 0 -> not available
} session;

/*
 * Orphan session detection process
 * Shim limits the number of simultaneous open sessions. Absent-minded or
 * malicious clients must be prevented from opening new sessions repeatedly
 * resulting in denial of service. Shim uses a lazy timeout mechanism to
 * detect unused sessions and reclaim them. It works like this:
 *
 * 1. The session time value is set to the current time when an API event
 *    finishes.
 * 2. If a new_session request fails to find any available session slots,
 *    it inspects the existing session time values for all the sessions,
 *    computing the difference between current time and the time value.
 *    If a session time difference exceeds TIMEOUT, then that session is
 *    cleaned up (cleanup_session), re-initialized, and returned as a
 *    new session. Queries are not cancelled though.
 *
 * Operations that are in-flight but may take an indeterminate amount of
 * time, for example PUT file uploads or execute_query statements, set their
 * time value to a point far in the future to protect them from harvesting.
 * Their time values are set to the current time when such operations complete.
 *
 */
enum mimetype
{ html, plain, binary };

const char SCIDB_HOST[] = "localhost";
int SCIDB_PORT = 1239;
session *sessions;            // Fixed pool of web client sessions
char *docroot;
static uid_t real_uid;        // For setting uid to logged in user when required
char *PAM_service_name = "login";       // Default PAM service name XXX make opt
/* Big common lock used to serialize global operations.
 * Each session also has a separate session lock. All the locks support
 * nesting/recursion.
 */
omp_lock_t biglock;
char *BASEPATH;
char *TMPDIR;                   // temporary files go here
token_list *tokens = NULL;      // the head of the list
token_list default_token;       // used by digest authentication
int MAX_SESSIONS;               // configurable maximum number of concurrent sessions
int SAVE_INSTANCE_ID;           // which instance ID should run save commands?
time_t TIMEOUT;                    // session timeout

int counter;                    // Used to label sessionid



/*
 * conn: A mongoose client connection
 * type: response mime type using the mimetype enumeration defined above
 * code: HTML integer response code (200=OK)
 * length: length of data buffer
 * data: character buffer to send
 * Write an HTTP response to client connection, it's OK for length=0 and
 * data=NULL.  This routine generates the http header.
 *
 * Response messages are http 1.1 with OK/ERROR header, content length, data.
 * example:
 * HTTP/1.1 <code> OK\r\n
 * Content-Length: <length>\r\n
 * Content-Type: text/html\r\n\r\n
 * <DATA>
 */
void
respond (struct mg_connection *conn, enum mimetype type, int code,
         size_t length, char *data)
{
  if (code != 200)  // error
    {
      if (data) // error with data payload (always presented as text/html here)
        {
          mg_printf (conn, "HTTP/1.1 %d ERROR\r\n"
                     "Content-Length: %lu\r\n"
                     "Cache-Control: no-cache\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Content-Type: text/html\r\n\r\n", code, strlen (data));
          mg_write (conn, data, strlen (data));
        }
      else                      // error without any payload
        mg_printf (conn,
                   "HTTP/1.1 %d ERROR\r\nCache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
                   code);
      return;
    }
  switch (type)
    {
    case html:
      mg_printf (conn, "HTTP/1.1 200 OK\r\n"
                 "Content-Length: %lu\r\n"
                 "Cache-Control: no-cache\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Content-Type: text/html\r\n\r\n", length);
      break;
    case plain:
      mg_printf (conn, "HTTP/1.1 200 OK\r\n"
                 "Content-Length: %lu\r\n"
                 "Cache-Control: no-cache\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Content-Type: text/plain\r\n\r\n", length);
      break;
    case binary:
      mg_printf (conn, "HTTP/1.1 200 OK\r\n"
                 "Content-Length: %lu\r\n"
                 "Cache-Control: no-cache\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Content-Type: application/octet-stream\r\n\r\n", length);
      break;
    }
  if (data)
    mg_write (conn, data, (size_t) length);
}

// Retrieve a session pointer from an id, return NULL if not found.
session *
find_session (int id)
{
  int j;
  session *ans = NULL;
  for (j = 0; j < MAX_SESSIONS; ++j)
    {
      if (sessions[j].sessionid != id)
        continue;
      ans =
        sessions[j].available == SESSION_UNAVAILABLE ? &sessions[j] : NULL;
    }
  return ans;
}

/* Cleanup a shim session and reset it to available.
 * Acquire the session lock before invoking this routine.
 */
void
cleanup_session (session * s)
{
  syslog (LOG_INFO, "cleanup_session releasing %d", s->sessionid);
  s->available = SESSION_AVAILABLE;
  s->queryid = 0;
  s->time = 0;
  if (s->pd > 0)
    close (s->pd);
  if (s->ibuf)
    {
      unlink (s->ibuf);
      free (s->ibuf);
      s->ibuf = NULL;
    }
  if (s->obuf)
    {
      syslog (LOG_INFO, "cleanup_session unlinking %s", s->obuf);
      unlink (s->obuf);
      free (s->obuf);
      s->obuf = NULL;
    }
  if (s->opipe)
    {
      syslog (LOG_INFO, "cleanup_session unlinking %s", s->opipe);
      unlink (s->opipe);
      free (s->opipe);
      s->opipe = NULL;
    }
}

/* Release a session defined in the client mg_request_info object 'id'
 * variable. Respond to the client connection conn.  Set resp to zero to not
 * respond to client conn, otherwise send http 200 response.
 */
void
release_session (struct mg_connection *conn, const struct mg_request_info *ri,
                 int resp)
{
  int id, k;
  char var1[MAX_VARLEN];
  syslog (LOG_INFO, "release_session");
  if (!ri->query_string)
    {
      respond (conn, plain, 400, 0, NULL);
      syslog (LOG_ERR, "release_session error invalid http query");
      return;
    }
  k = strlen (ri->query_string);
  mg_get_var (ri->query_string, k, "id", var1, MAX_VARLEN);
  id = atoi (var1);
  session *s = find_session (id);
  if (s)
    {
      if(s->stream > 0) // Not allowed!
      {
        respond (conn, plain, 405, 0, NULL);
        syslog (LOG_ERR, "release_session not allowed error");
        return;
      }
      omp_set_lock (&s->lock);
      cleanup_session (s);
      omp_unset_lock (&s->lock);
      if (resp)
        respond (conn, plain, 200, 0, NULL);
    }
  else if (resp)
    respond (conn, plain, 404, 0, NULL);        // not found
}

/* Note: cancel_query does not trigger a cleanup_session for the session
 * corresponding to the query. The client that initiated the original query is
 * still responsible for session cleanup.
 */
void
cancel_query (struct mg_connection *conn, const struct mg_request_info *ri)
{
  int id, k;
  void *can_con;
  char var1[MAX_VARLEN];
  char SERR[MAX_VARLEN];
  if (!ri->query_string)
    {
      respond (conn, plain, 400, 0, NULL);
      syslog (LOG_ERR, "cancel_query error invalid http query");
      return;
    }
  k = strlen (ri->query_string);
  mg_get_var (ri->query_string, k, "id", var1, MAX_VARLEN);
  id = atoi (var1);
  session *s = find_session (id);
  if (s && s->queryid > 0)
    {
      syslog (LOG_INFO, "cancel_query %d %llu", id, s->queryid);
      if (s->con)
        {
// Establish a new SciDB context used to issue the cancel query.
          can_con = scidbconnect (SCIDB_HOST, SCIDB_PORT);
// check for valid context from scidb
          if (!can_con)
            {
              syslog (LOG_ERR,
                      "cancel_query error could not connect to SciDB");
              respond (conn, plain, 503,
                       strlen ("Could not connect to SciDB"),
                       "Could not connect to SciDB");
              return;
            }

          memset (var1, 0, MAX_VARLEN);
          snprintf (var1, MAX_VARLEN, "cancel(%llu)", s->queryid);
          memset (SERR, 0, MAX_VARLEN);
          executeQuery (can_con, var1, 1, SERR);
          syslog (LOG_INFO, "cancel_query %s", SERR);
          scidbdisconnect (can_con);
        }
      time (&s->time);
      respond (conn, plain, 200, 0, NULL);
    }
  else
    respond (conn, plain, 404, 0, NULL);        // not found
}


/* Initialize a session. Obtain the big lock before calling this.
 * returns 1 on success, 0 otherwise.
 */
int
init_session (session * s)
{
  int fd;
  omp_set_lock (&s->lock);
  s->sessionid = counter;
  counter++;
  if(counter<0) counter=1;
  s->ibuf  = (char *) malloc (PATH_MAX);
  s->obuf  = (char *) malloc (PATH_MAX);
  s->opipe = (char *) malloc (PATH_MAX);
  snprintf (s->ibuf, PATH_MAX, "%s/shim_input_buf_XXXXXX", TMPDIR);
  snprintf (s->obuf, PATH_MAX, "%s/shim_output_buf_XXXXXX", TMPDIR);
  snprintf (s->opipe, PATH_MAX, "%s/shim_output_pipe_XXXXXX", TMPDIR);
// Set up the input buffer
  fd = mkstemp (s->ibuf);
// XXX We need to make it so that whoever runs scidb can R/W to this file.
// Since, in general, we don't know who is running scidb (anybody can), and, in
// general, shim runs as a service we have a mismatch. Right now, we set it so
// that anyone can write to these to work around this. But really, these files
// should be owned by the scidb process user. Hmmm.  (See also below for output
// buffer and pipe.)
  chmod (s->ibuf, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
  if (fd > 0)
    close (fd);
  else
    {
      syslog (LOG_ERR, "init_session can't create file");
      cleanup_session (s);
      omp_unset_lock (&s->lock);
      return 0;
    }
// Set up the output buffer
  s->pd = 0;
// Set default behavior to not stream
  s->stream = 0;
  s->save = 0;
  s->compression = -1;
  fd = mkstemp (s->obuf);
  chmod (s->obuf, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
  if (fd > 0)
    close (fd);
  else
    {
      syslog (LOG_ERR, "init_session can't create file");
      cleanup_session (s);
      omp_unset_lock (&s->lock);
      return 0;
    }

/* Set up the output pipe
 * We need to create a unique name for the pipe.  Since mkstemp can only be
 * used to create files, we will create the pipe with a generic name, then use
 * mkstemp to create file with a unique name, then rename the pipe on top of
 * the file.
 */
  fd = mkstemp (s->opipe);
  if (fd >= 0)
    close (fd);
  else
    {
      syslog (LOG_ERR, "init_session can't create pipefile");
      cleanup_session (s);
      omp_unset_lock (&s->lock);
      return 0;
    }
  char* pipename;
  pipename = (char *) malloc (PATH_MAX);
  snprintf (pipename, PATH_MAX, "%s/shim_generic_pipe%d", TMPDIR,s->sessionid);
  syslog (LOG_ERR, "creating generic pipe: %s", pipename);
  fd = mkfifo (pipename, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
  chmod(pipename, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
  if (fd != 0)
    {
      syslog (LOG_ERR, "init_session can't create pipe, error");
      cleanup_session (s);
      free(pipename);
      omp_unset_lock (&s->lock);
      return 0;
    }
  fd = rename(pipename, s->opipe);
  if (fd!=0)
    {
      syslog (LOG_ERR, "init_session can't rename pipe");
      unlink(pipename);
      free(pipename);
      cleanup_session (s);
      omp_unset_lock (&s->lock);
      return 0;      
    }
  free(pipename);

  time (&s->time);
  s->available = SESSION_UNAVAILABLE;
  omp_unset_lock (&s->lock);
  return 1;
}

/* Find an available session.  If no sessions are available, return -1.
 * Otherwise, initialize I/O buffers and return the session array index.
 * We acquire the big lock on the session list here--only one thread at
 * a time is allowed to run this.
 */
int
get_session ()
{
  int k, j, id = -1;
  time_t t;
  omp_set_lock (&biglock);
  for (j = 0; j < MAX_SESSIONS; ++j)
    {
      if (sessions[j].available == SESSION_AVAILABLE)
        {
          k = init_session(&sessions[j]);
          if (k > 0)
            {
              id = j;
              break;
            }
        }
    }
  if (id < 0)
    {
      time (&t);
      /* Couldn't find any available sessions. Check for orphans. */
      for (j = 0; j < MAX_SESSIONS; ++j)
        {
          if (t - sessions[j].time > TIMEOUT)
            {
              syslog (LOG_INFO, "get_session reaping session %d", j);
              omp_set_lock (&sessions[j].lock);
              cleanup_session (&sessions[j]);
              omp_unset_lock (&sessions[j].lock);
              if (init_session (&sessions[j]) > 0)
                {
                  id = j;
                  break;
                }
            }
        }
    }
  omp_unset_lock (&biglock);
  return id;
}

/* Authenticate a user with PAM and return a token.
 *
 * Error 400 is returned if the query string is empty or if this is not a
 * TLS/SSL connection.  login expects two query string arguments, username and
 * password.  Error 401 is returned if PAM can't authenticate the
 * username/password combo.  If successful, a token is returned. Use the token
 * with other TLS connections to show authentication.
 */
void
login (struct mg_connection *conn, const struct mg_request_info *ri)
{
  int k;
  uid_t uid;
  char u[MAX_VARLEN];
  char p[MAX_VARLEN];
  token_list *t = NULL;
  if (!ri->query_string || !ri->is_ssl)
    {
      respond (conn, plain, 400, 0, NULL);
      syslog (LOG_ERR, "login error invalid http query");
      return;
    }
  k = strlen (ri->query_string);
  mg_get_var (ri->query_string, k, "username", u, MAX_VARLEN);
  mg_get_var (ri->query_string, k, "password", p, MAX_VARLEN);
  k = do_pam_login (PAM_service_name, u, p);
  if (k == 0)
    {
/* Success. Generate a new auth token and return it.
 * XXX NOTE! No limit to the number of authenticated logins.  This means a
 * careless user can extend the token list indefinitely (that is, leak).
 * FIX ME
 */
      omp_set_lock (&biglock);
      uid = username2uid (u);
      while (t == NULL)
        t = addtoken (tokens, authtoken (), uid);
      tokens = t;
      omp_unset_lock (&biglock);
      memset (p, 0, MAX_VARLEN);
      snprintf (p, MAX_VARLEN, "%lu", t->val);
      syslog (LOG_INFO, "Authenticated user %s token %s uid %ld", u, p,
              (long) uid);
      respond (conn, plain, 200, strlen (p), p);
    }
  else
    {
      syslog (LOG_INFO, "PAM login for username %s returned %d", u, k);
      respond (conn, plain, 401, 0, NULL);      // Not authorized.
    }
  return;
}

/* Remove an authentication token from the tokens list.
 *
 * Error 400 is returned if the query string is empty or if this is not a
 * TLS/SSL connection.
 * Expects query string argument named 'auth.'
 * 200 is returned with no message on success.
 */
void
logout (struct mg_connection *conn, const struct mg_request_info *ri)
{
  int k;
  unsigned long l;
  char a[MAX_VARLEN];
  if (!ri->query_string || !ri->is_ssl)
    {
      respond (conn, plain, 400, 0, NULL);
      syslog (LOG_ERR, "logout error invalid http query");
      return;
    }
  k = strlen (ri->query_string);
  mg_get_var (ri->query_string, k, "auth", a, MAX_VARLEN);
  l = strtoul (a, NULL, 0);
  omp_set_lock (&biglock);
  tokens = removetoken (tokens, l);
  omp_unset_lock (&biglock);
  respond (conn, plain, 200, 0, NULL);
  return;
}



/* Client file upload
 * POST upload to server-side file defined in the session identified
 * by the 'id' variable in the mg_request_info query string.
 * Respond to the client connection as follows:
 * 200 success, <uploaded filename>\r\n returned in body
 * 404 session not found
 */
void
upload (struct mg_connection *conn, const struct mg_request_info *ri)
{
  int id, k;
  session *s;
  char var1[MAX_VARLEN];
  char buf[MAX_VARLEN];
  if (!ri->query_string)
    {
      respond (conn, plain, 400, 0, NULL);
      syslog (LOG_INFO, "upload error invalid http query");
      return;
    }
  k = strlen (ri->query_string);
  mg_get_var (ri->query_string, k, "id", var1, MAX_VARLEN);
  id = atoi (var1);
  s = find_session (id);
  if (s)
    {
      omp_set_lock (&s->lock);
      s->time = time (NULL) + WEEK;     // Upload should take less than a week!
      mg_append (conn, s->ibuf);
      time (&s->time);
      snprintf (buf, MAX_VARLEN, "%s\r\n", s->ibuf);
// XXX if mg_append fails, report server error too
      respond (conn, plain, 200, strlen (buf), buf);    // XXX report bytes uploaded
      omp_unset_lock (&s->lock);
    }
  else
    {
      respond (conn, plain, 404, 0, NULL);      // not found
    }
  return;
}

/* Obtain a new session for the mongoose client in conn.
 * Respond as follows:
 * 200 OK
 * session ID
 * --or--
 * 503 ERROR (out of resources)
 *
 * An error usually means all the sessions are consumed.
 */
void
new_session (struct mg_connection *conn)
{
  char buf[MAX_VARLEN];
  int j = get_session ();
  syslog (LOG_INFO, "new_session %d", j);
  if (j > -1)
    {
      syslog (LOG_INFO, "new_session session id=%d ibuf=%s obuf=%s opipe=%s",
              sessions[j].sessionid, sessions[j].ibuf, sessions[j].obuf,
              sessions[j].opipe);
      snprintf (buf, MAX_VARLEN, "%d\r\n", sessions[j].sessionid);
      respond (conn, plain, 200, strlen (buf), buf);
    }
  else
    {
      respond (conn, plain, 503, 0, NULL);      // out of resources
    }
}


/* Return shim's version build
 * Respond as follows:
 * 200 OK
 * version ID
 *
 * An error usually means all the sessions are consumed.
 */
void
version (struct mg_connection *conn)
{
  char buf[MAX_VARLEN];
  syslog (LOG_INFO, "version \n");
  snprintf (buf, MAX_VARLEN, "%s\r\n", VERSION);
  respond (conn, plain, 200, strlen (buf), buf);
}

#ifdef DEBUG
void
debug (struct mg_connection *conn)
{
  int j;
  char buf[MAX_VARLEN];
  char *p = buf;
  size_t l, k = MAX_VARLEN;
  syslog (LOG_INFO, "debug \n");
  omp_set_lock (&biglock);
  for (j = 0; j < MAX_SESSIONS; ++j)
  {
    l = snprintf (p, k, "j=%d id=%d a=%d p=%s\n",j,sessions[j].sessionid, sessions[j].available,sessions[j].opipe);
    k = k - l;
    if(k<=0) break;
    p = p + l;
  }
  omp_unset_lock (&biglock);
  respond (conn, plain, 200, strlen (buf), buf);
}
#endif


/* Read bytes from a query result output buffer.
 * The mg_request_info must contain the keys:
 * n=<max number of bytes to read -- signed int32>
 * id=<session id>
 * Writes at most n bytes back to the client or a 416 error if something
 * went wrong.
 */
void
readbytes (struct mg_connection *conn, const struct mg_request_info *ri)
{
  int id, k, n, pl, l;
  session *s;
  struct pollfd pfd;
  char *buf;
  char var[MAX_VARLEN];
  struct stat st;
  if (!ri->query_string)
    {
      respond (conn, plain, 400, 0, NULL);
      syslog (LOG_ERR, "readbytes error invalid http query");
      return;
    }
  syslog (LOG_INFO, "readbytes querystring %s", ri->query_string);
  k = strlen (ri->query_string);
  mg_get_var (ri->query_string, k, "id", var, MAX_VARLEN);
  id = atoi (var);
  s = find_session (id);
  if (!s)
    {
      syslog (LOG_INFO, "readbytes session error");
      respond (conn, plain, 404, 0, NULL);
      return;
    }
  if (!s->save)
    {
      respond (conn, plain, 404, 0, NULL);
      syslog (LOG_ERR, "readlines query output not saved");
      return;
    }
  omp_set_lock (&s->lock);
// Check to see if the output buffer is open for reading, if not do so.
  if (s->pd < 1)
    {
      s->pd = 
          s->stream ? 
          open(s->opipe, O_RDONLY) :
          open (s->obuf, O_RDONLY | O_NONBLOCK);

      if (s->pd < 1)
        {
          syslog (LOG_ERR, "readbytes error opening output buffer");
          respond (conn, plain, 500, 0, NULL);
          omp_unset_lock (&s->lock);
          return;
        }
    }
  memset (var, 0, MAX_VARLEN);
// Retrieve max number of bytes to read
  mg_get_var (ri->query_string, k, "n", var, MAX_VARLEN);
  n = atoi (var);
  if (n < 1)
    {
      syslog (LOG_INFO, "readbytes id=%d returning entire buffer",id);
      if (s->stream)
        mg_send_pipe (conn, s->opipe, s->stream, s->compression);
      else
        mg_send_file (conn, s->obuf);
      omp_unset_lock (&s->lock);
      syslog (LOG_INFO, "readbytes id=%d done",id);
      return;
    }
  if (n > MAX_RETURN_BYTES)
    n = MAX_RETURN_BYTES;
  if(fstat(s->pd, &st) < 0)
    {
      syslog (LOG_ERR, "fstat error");
      respond (conn, plain, 507, 0, NULL);
      omp_unset_lock (&s->lock);
      return;
    }
  if((off_t)n > st.st_size)
    n = (int) st.st_size;

  buf = (char *) malloc (n);
  if (!buf)
    {
      syslog (LOG_ERR, "readbytes out of memory");
      respond (conn, plain, 507, 0, NULL);
      omp_unset_lock (&s->lock);
      return;
    }

  pfd.fd = s->pd;
  pfd.events = POLLIN;
  pl = 0;
  while (pl < 1)
    {
      pl = poll (&pfd, 1, 250);
      if (pl < 0)
        break;
    }

  l = (int) read (s->pd, buf, n);
  syslog (LOG_INFO, "readbytes  read %d n=%d", l, n);
  if (l < 1)   // EOF or error
    {
      free (buf);
      respond (conn, plain, 416, 0, NULL);
      omp_unset_lock (&s->lock);
      return;
    }
  respond (conn, binary, 200, l, buf);
  free (buf);
  time (&s->time);
  omp_unset_lock (&s->lock);
}



/* Read ascii lines from a query result on the query pipe.
 * The mg_request_info must contain the keys:
 * n = <max number of lines to read>
 *     Set n to zero to just return all the data. n>0 allows
 *     repeat calls to this function to iterate through data n lines at a time
 * id = <session id>
 * Writes at most n lines back to the client or a 419 error if something
 * went wrong or end of file.
 */
void
readlines (struct mg_connection *conn, const struct mg_request_info *ri)
{
  int id, k, n, pl;
  ssize_t l;
  size_t m, t, v;
  session *s;
  char *lbuf, *buf, *p, *tmp;
  struct pollfd pfd;
  char var[MAX_VARLEN];
  syslog (LOG_INFO, "readlines");
  if (!ri->query_string)
    {
      respond (conn, plain, 400, 0, NULL);
      syslog (LOG_ERR, "readlines error invalid http query");
      return;
    }
  k = strlen (ri->query_string);
  mg_get_var (ri->query_string, k, "id", var, MAX_VARLEN);
  id = atoi (var);
  s = find_session (id);
  if (!s)
    {
      respond (conn, plain, 404, 0, NULL);
      syslog (LOG_ERR, "readlines error invalid session");
      return;
    }
  if (!s->save)
    {
      respond (conn, plain, 404, 0, NULL);
      syslog (LOG_ERR, "readlines query output not saved");
      return;
    }
// Retrieve max number of lines to read
  mg_get_var (ri->query_string, k, "n", var, MAX_VARLEN);
  n = atoi (var);
// Check to see if client wants the whole file at once, if so return it.
  omp_set_lock (&s->lock);
  if ((n < 1) || s->stream)
    {
      syslog (LOG_INFO, "readlines returning entire buffer");
      if (s->stream)
        mg_send_pipe (conn, s->opipe, s->stream, s->compression);
      else
        mg_send_file (conn, s->obuf);

      omp_unset_lock (&s->lock);
      return;
    }
// Check to see if output buffer is open for reading
  syslog (LOG_INFO, "readlines opening buffer");
  if (s->pd < 1)
    {
      s->pd = open (s->stream ? s->opipe : s->obuf, O_RDONLY | O_NONBLOCK);
      if (s->pd > 0)
        s->pf = fdopen (s->pd, "r");
      if (s->pd < 1 || !s->pf)
        {
          respond (conn, plain, 500, 0, NULL);
          syslog (LOG_ERR, "readlines error opening output buffer");
          omp_unset_lock (&s->lock);
          return;
        }
    }
  memset (var, 0, MAX_VARLEN);
  if (n * MAX_VARLEN > MAX_RETURN_BYTES)
    {
      n = MAX_RETURN_BYTES / MAX_VARLEN;
    }

  k = 0;
  t = 0;                        // Total bytes read so far
  m = MAX_VARLEN;
  v = m * n;                    // Output buffer size
  lbuf = (char *) malloc (m);
  if (!lbuf)
    {
      respond (conn, plain, 500, strlen ("Out of memory"), "Out of memory");
      syslog (LOG_ERR, "readlines out of memory");
      omp_unset_lock (&s->lock);
      return;
    }
  buf = (char *) malloc (v);
  if (!buf)
    {
      free (lbuf);
      respond (conn, plain, 500, strlen ("Out of memory"), "Out of memory");
      syslog (LOG_ERR, "readlines out of memory");
      omp_unset_lock (&s->lock);
      return;
    }
  while (k < n)
    {
      memset (lbuf, 0, m);
      pfd.fd = s->pd;
      pfd.events = POLLIN;
      pl = 0;
      while (pl < 1)
        {
          pl = poll (&pfd, 1, 250);
          if (pl < 0)
            break;
        }

      l = getline (&lbuf, &m, s->pf);
      if (l < 0)
        {
          break;
        }
// Check if buffer is big enough to contain next line
      if (t + l > v)
        {
          v = 2 * v;
          tmp = realloc (buf, v);
          if (!tmp)
            {
              free (lbuf);
              free (buf);
              respond (conn, plain, 500, strlen ("Out of memory"),
                       "Out of memory");
              syslog (LOG_ERR, "readlines out of memory");
              omp_unset_lock (&s->lock);
              return;
            }
          else
            {
              buf = tmp;
            }
        }
// Copy line into buffer
      p = buf + t;
      t += l;
      memcpy (p, lbuf, l);
      k += 1;
    }
  free (lbuf);
  if (t == 0)
    {
      syslog (LOG_INFO, "readlines EOF");
      respond (conn, plain, 410, 0, NULL);      // gone--i.e. EOF
    }
  else
    respond (conn, plain, 200, t, buf);
  free (buf);
  time (&s->time);
  omp_unset_lock (&s->lock);
}

/* execute_query blocks until the query is complete. However, if stream is
 * specified, execute_query releases the lock and immediately replies to the
 * client with a query ID so that the client can wait for data, then
 * execute_query * proceeds normally until the query ends.
 * This function always disconnects from SciDB after completeQuery finishes.
 * query string variables:
 * id=<session id> (required)
 * query=<query string> (required)
 * release={0 or 1}  (optional, default 0)
 *   release > 0 invokes release_session after completeQuery.
 * save=<format string> (optional, default 0-length string)
 *   set the save format to something to wrap the query in a save
 * stream={0,1,2} (optional, default 0)
 *   stream = 0 indicates no streaming, save query output to server file
 *   stream = 1 indicates stream output through server named pipe
 *   stream = 2 indicates stream compressed output through server named pipe
 * compression={-1,0,1,...,9}
 *   optional compression level when stream=2. If compression>=0, then this
 *   automatically sets stream=2.
 *
 * Any error that occurs during execute_query that is associated
 * with a valid session ID results in the release of the session.
 */
void
execute_query (struct mg_connection *conn, const struct mg_request_info *ri)
{
  int id, k, rel = 0, stream = 0, compression = -1, pipefd;
  unsigned long long l;
  session *s;
  char var[MAX_VARLEN];
  char buf[MAX_VARLEN];
  char save[MAX_VARLEN];
  char SERR[MAX_VARLEN];
  char *qrybuf, *qry;
  struct prep pq;               // prepared query storage

  if (!ri->query_string)
    {
      respond (conn, plain, 400, strlen ("Invalid http query"),
               "Invalid http query");
      syslog (LOG_ERR, "execute_query error invalid http query string");
      return;
    }
  k = strlen (ri->query_string);
  mg_get_var (ri->query_string, k, "id", var, MAX_VARLEN);
  id = atoi (var);
  memset (var, 0, MAX_VARLEN);
  mg_get_var (ri->query_string, k, "release", var, MAX_VARLEN);
  if (strlen (var) > 0)
    rel = atoi (var);
  memset (var, 0, MAX_VARLEN);
  mg_get_var (ri->query_string, k, "stream", var, MAX_VARLEN);
  if (strlen (var) > 0)
    stream = atoi (var);
  memset (var, 0, MAX_VARLEN);
  mg_get_var (ri->query_string, k, "compression", var, MAX_VARLEN);
  if (strlen (var) > 0)
  {
    compression = atoi (var);
  }
  if(compression> -1) stream=2;
  syslog (LOG_INFO, "execute_query for session id %d", id);
  s = find_session (id);
  if (!s)
    {
      syslog (LOG_ERR, "execute_query error Invalid session ID %d", id);
      respond (conn, plain, 404, strlen ("Invalid session ID"),
               "Invalid session ID");
      return;                   // check for valid session
    }
  qrybuf = (char *) malloc (k);
  if (!qrybuf)
    {
      syslog (LOG_ERR, "execute_query error out of memory");
      respond (conn, plain, 500, strlen ("Out of memory"), "Out of memory");
      omp_set_lock (&s->lock);
      cleanup_session (s);
      omp_unset_lock (&s->lock);
      return;
    }
  qry = (char *) malloc (k + MAX_VARLEN);
  if (!qry)
    {
      free (qrybuf);
      syslog (LOG_ERR, "execute_query error out of memory");
      respond (conn, plain, 500, strlen ("Out of memory"), "Out of memory");
      omp_set_lock (&s->lock);
      cleanup_session (s);
      omp_unset_lock (&s->lock);
      return;
    }
  if (stream)
    {
      syslog (LOG_INFO, "execute_query stream indicated");
    }
  if (stream>1)
    {
      syslog (LOG_INFO, "gzip compressed stream indicated, compression level %d", compression);
    }
  omp_set_lock (&s->lock);
  memset (var, 0, MAX_VARLEN);
  mg_get_var (ri->query_string, k, "save", save, MAX_VARLEN);
  mg_get_var (ri->query_string, k, "query", qrybuf, k);
// If save is indicated, modify query
  if (strlen (save) > 0)
  {
    s->save = 1;
    snprintf (qry, k + MAX_VARLEN, "save(%s,'%s',%d,'%s')", qrybuf,
              stream ? s->opipe : s->obuf, SAVE_INSTANCE_ID, save);
  }
  else
  {
    s->save = 0;
    snprintf (qry, k + MAX_VARLEN, "%s", qrybuf);
  }

  if (!s->con)
    s->con = scidbconnect (SCIDB_HOST, SCIDB_PORT);
  syslog (LOG_INFO, "execute_query %d s->con = %p %s", id, s->con, qry);
  if (!s->con)
    {
      free (qry);
      free (qrybuf);
      syslog (LOG_ERR, "execute_query error could not connect to SciDB");
      respond (conn, plain, 503, strlen ("Could not connect to SciDB"),
                 "Could not connect to SciDB");
      cleanup_session (s);
      omp_unset_lock (&s->lock);
      return;
    }

  syslog (LOG_INFO, "execute_query %d connected", id);
  prepare_query (&pq, s->con, qry, 1, SERR);
  l = pq.queryid;
  syslog (LOG_INFO, "execute_query id=%d scidb queryid = %llu", id, l);
  if (l < 1 || !pq.queryresult)
    {
      free (qry);
      free (qrybuf);
      syslog (LOG_ERR, "execute_query error %s", SERR);
      respond (conn, plain, 500, strlen (SERR), SERR);
      if (s->con)
        scidbdisconnect (s->con);
      s->con = NULL;
      cleanup_session (s);
      omp_unset_lock (&s->lock);
      return;
    }
/* Set the queryID for potential future cancel event.
 * The time flag is set to a future value to prevent get_session from
 * declaring this session orphaned while a query is running. This
 * session cannot be reclaimed until the query finishes, since the
 * lock is held.
 */
  s->queryid = l;
  s->time = time (NULL) + WEEK;
  s->stream = stream;
  s->compression = compression;
  if (s->con)
    {
      if (stream)
      {
/* Respond with the query ID before it runs.
 * We have to release the lock to allow a client to run read_bytes or
 * read_lines to pull from the pipe. Also set rel=1 in case it isn't
 * already.
 */
        rel = 1;
        omp_unset_lock (&s->lock);
        snprintf (buf, MAX_VARLEN, "%llu", l);        // Return the query ID
        respond (conn, plain, 200, strlen (buf), buf);
// DEBUG sleep(3);
      }
      l = execute_prepared_query (s->con, qry, &pq, 1, SERR);
/* execute_prepared_query blocks until the query completes or an error
 * occurs. That means that if l > 0, then by the time we get here the
 * client read_bytes is done and we can safely re-acquire the lock
 * without risking deadlock. If l < 1, then an error occurred and we
 * need to clean up.
 */
/* Force the client thread to terminate because of error */
      if(stream && (l < 1))
      {
        syslog(LOG_ERR, "streaming error %llu, shutting down pipe",l);
        pipefd = open(s->opipe, O_WRONLY);
        close(pipefd);
        unlink (s->opipe);
      }
      if (stream)
      {
        omp_set_lock (&s->lock);
      }
    }
  if (l < 1) // something went wrong
    {
      free (qry);
      free (qrybuf);
      syslog (LOG_ERR, "execute_prepared_query error %s", SERR);
      if(!stream)
        respond (conn, plain, 500, strlen (SERR), SERR);
      if (s->con)
        scidbdisconnect (s->con);
      s->con = NULL;
      cleanup_session (s);
      omp_unset_lock (&s->lock);
      return;
    }
  if (s->con)
    completeQuery (l, s->con, SERR);

  free (qry);
  free (qrybuf);
  syslog (LOG_INFO, "execute_query %d done, disconnecting", s->sessionid);
  if (s->con)
    scidbdisconnect (s->con);
  s->con = NULL;
  if (rel > 0)
    {
      syslog (LOG_INFO, "execute_query releasing HTTP session %d",
              s->sessionid);
      cleanup_session (s);
    }
  time (&s->time);
  omp_unset_lock (&s->lock);
// Respond to the client
  if(!stream)
  {
    snprintf (buf, MAX_VARLEN, "%llu", l);        // Return the query ID
    respond (conn, plain, 200, strlen (buf), buf);
  }
}


// Control API ---------------------------------------------------------------
void
stopscidb (struct mg_connection *conn, const struct mg_request_info *ri)
{
  int k;
  char var[MAX_VARLEN];
  char cmd[2 * MAX_VARLEN];
  k = strlen (ri->query_string);
  mg_get_var (ri->query_string, k, "db", var, MAX_VARLEN);
  syslog (LOG_INFO, "stopscidb %s", var);
  snprintf (cmd, 2 * MAX_VARLEN, "scidb.py stopall %s", var);
  k = system (cmd);
  respond (conn, plain, 200, 0, NULL);
}

void
startscidb (struct mg_connection *conn, const struct mg_request_info *ri)
{
  int k;
  char var[MAX_VARLEN];
  char cmd[2 * MAX_VARLEN];
  k = strlen (ri->query_string);
  mg_get_var (ri->query_string, k, "db", var, MAX_VARLEN);
  syslog (LOG_INFO, "startscidb %s", var);
  snprintf (cmd, 2 * MAX_VARLEN, "scidb.py startall %s", var);
  k = system (cmd);
  respond (conn, plain, 200, 0, NULL);
}

void
getlog (struct mg_connection *conn, const struct mg_request_info *ri)
{
  syslog (LOG_INFO, "getlog");
  system
    ("tail -n 1555 `ps axu | grep SciDB | grep \"\\/000\\/0\"  | grep SciDB | head -n 1 | sed -e \"s/SciDB-000.*//\" | sed -e \"s/.* \\//\\//\"`/scidb.log > /tmp/.scidb.log");
  mg_send_file (conn, "/tmp/.scidb.log");
}

/* Check authentication token to see if it's in our list. */
token_list *
check_auth (token_list * head, struct mg_connection *conn,
            const struct mg_request_info *ri)
{
  int k;
  unsigned long l;
  char var[MAX_VARLEN];
  token_list *t = head;
/* Check for basic digest authentication first */
  if(mg_get_basic_auth(conn)==1)
  {
    return &default_token;
  }
  if (!ri->query_string)
    {
      respond (conn, plain, 400, 0, NULL);
      syslog (LOG_ERR, "authentication error");
      return NULL;
    }

  k = strlen (ri->query_string);
  mg_get_var (ri->query_string, k, "auth", var, MAX_VARLEN);
  l = strtoul (var, NULL, 0);
/* Scan the list for a match */
  while (t)
    {
      if (t->val == l)
        {
/* Authorized, we don't repond here since a downstream callback will */
          return t;
        }
      t = (token_list *) t->next;
    }
/* Not authorized */
  respond (conn, plain, 401, 0, NULL);
  return NULL;
}


/* Mongoose generic begin_request callback; we dispatch URIs to their
 * appropriate handlers.
 */
static int
begin_request_handler (struct mg_connection *conn)
{
  char buf[MAX_VARLEN];
  const struct mg_request_info *ri = mg_get_request_info (conn);
  token_list *tok = NULL;

// Don't log login query string
  if (strcmp (ri->uri, "/measurement") == 0)
    {
    }
  else if (strcmp (ri->uri, "/login") == 0)
    syslog (LOG_INFO, "%s", ri->uri);
  else if (ri->is_ssl)
    syslog (LOG_INFO, "(SSL) %s?%s", ri->uri, ri->query_string);
  else
    syslog (LOG_INFO, "%s?%s", ri->uri, ri->query_string);

/* Check API authentication (encrypted sessions only--only applies to
 * the listed subset of the available shim API--API services not listed
 * below work over https without needing auth)
 */
  if (ri->is_ssl &&
      (!strcmp (ri->uri, "/new_session") ||
       !strcmp (ri->uri, "/upload_file") ||
       !strcmp (ri->uri, "/read_lines") ||
       !strcmp (ri->uri, "/read_bytes") ||
       !strcmp (ri->uri, "/execute_query") ||
       !strcmp (ri->uri, "/cancel")) &&
      !(tok = check_auth (tokens, conn, ri)))
    goto end;

// CLIENT API
  if (!strcmp (ri->uri, "/new_session"))
    new_session (conn);
  else if (!strcmp (ri->uri, "/version"))
    version (conn);
#ifdef DEBUG
  else if (!strcmp (ri->uri, "/debug"))
    debug (conn);
#endif
  else if (!strcmp (ri->uri, "/login"))
    login (conn, ri);
  else if (!strcmp (ri->uri, "/logout"))
    logout (conn, ri);
  else if (!strcmp (ri->uri, "/release_session"))
    release_session (conn, ri, 1);
  else if (!strcmp (ri->uri, "/upload_file"))
    upload (conn, ri);
  else if (!strcmp (ri->uri, "/read_lines"))
    readlines (conn, ri);
  else if (!strcmp (ri->uri, "/read_bytes"))
    readbytes (conn, ri);
  else if (!strcmp (ri->uri, "/execute_query"))
    execute_query (conn, ri);
  else if (!strcmp (ri->uri, "/cancel"))
    cancel_query (conn, ri);
// CONTROL API
//      else if (!strcmp (ri->uri, "/stop_scidb"))
//        stopscidb (conn, ri);
//      else if (!strcmp (ri->uri, "/start_scidb"))
//        startscidb (conn, ri);
  else if (!strcmp (ri->uri, "/get_log"))
    getlog (conn, ri);
  else
    {
// fallback to http file server
      if (strstr (ri->uri, ".htpasswd"))
        {
          syslog (LOG_ERR, "client trying to read password file");
          respond (conn, plain, 401, strlen ("Not authorized"),
                   "Not authorized");
          goto end;
        }
      if (!strcmp (ri->uri, "/"))
        snprintf (buf, MAX_VARLEN, "%s/index.html", docroot);
      else
        snprintf (buf, MAX_VARLEN, "%s/%s", docroot, ri->uri);
      mg_send_file (conn, buf);
    }

end:
// Mark as processed by returning non-null value.
  return 1;
}


/* Parse the command line options, updating the options array */
void
parse_args (char **options, int argc, char **argv, int *daemonize)
{
  int c;
  while ((c = getopt (argc, argv, "hvfn:p:r:s:t:m:o:i:")) != -1)
    {
      switch (c)
        {
        case 'h':
          printf
            ("Usage:\nshim [-h] [-v] [-f] [-n <PAM service name>] [-p <http port>] [-r <document root>] [-s <scidb port>] [-t <tmp I/O DIR>] [-m <max concurrent sessions] [-o http session timeout] [-i instance id for save]\n");
          printf
            ("The -v option prints the version build ID and exits.\nSpecify -f to run in the foreground.\nDefault http ports are 8080 and 8083(SSL).\nDefault SciDB port is 1239.\nDefault document root is /var/lib/shim/wwwroot.\nDefault PAM service name is 'login'.\nDefault temporary I/O directory is /tmp.\nDefault max concurrent sessions is 50 (max 100).\nDefault http session timeout is 60s and min is 60 (see API doc).\nDefault instance id for save to file is 0.\n");
          printf
            ("Start up shim and view http://localhost:8080/api.html from a browser for help with the API.\n\n");
          exit (0);
          break;
        case 'v':
          printf ("%s\n", VERSION);
          exit (0);
          break;
        case 'f':
          *daemonize = 0;
          break;
        case 'n':
          PAM_service_name = optarg;
          break;
        case 'p':
          options[1] = optarg;
          break;
        case 'r':
          options[3] = optarg;
          memset(options[5],0,PATH_MAX);
          strncat (options[5], optarg, PATH_MAX);
          strncat (options[5], "/../ssl_cert.pem", PATH_MAX - 17);
          break;
        case 's':
          SCIDB_PORT = atoi (optarg);
          break;
        case 't':
          TMPDIR = optarg;
          break;
        case 'i':
          SAVE_INSTANCE_ID = atoi(optarg);
          SAVE_INSTANCE_ID = (SAVE_INSTANCE_ID < 0 ? 0 : SAVE_INSTANCE_ID);
          break;
        case 'm':
          MAX_SESSIONS = atoi(optarg);
          MAX_SESSIONS = (MAX_SESSIONS > 100 ? 100 : MAX_SESSIONS);
          break;
        case 'o':
          TIMEOUT = atoi(optarg);
          TIMEOUT = (TIMEOUT < 60 ? 60 : TIMEOUT);
          break;
        default:
          break;
        }
    }
}

static void
signalHandler (int sig)
{
  /* catch termination signals and shut down gracefully */
   int j;
  signal (sig, signalHandler);
  omp_set_lock (&biglock);
  for (j = 0; j < MAX_SESSIONS; ++j)
    {
      syslog (LOG_INFO, "terminating, reaping session %d", j);
/* note: we intentionally forget about acquiring locks here,
 * we are about to exit!
 */
      cleanup_session (&sessions[j]);
    }
  omp_unset_lock (&biglock);
  exit (0);
}

int
main (int argc, char **argv)
{
  int j, k, l, daemonize = 1;
  char *cp, *ports;
  struct mg_context *ctx;
  struct mg_callbacks callbacks;
  struct stat check_ssl;
  struct rlimit resLimit = { 0 };
  char pbuf[MAX_VARLEN];
  char *options[9];
  options[0] = "listening_ports";
  options[1] = DEFAULT_HTTP_PORT;
  options[2] = "document_root";
  options[3] = "/var/lib/shim/wwwroot";
  options[4] = "ssl_certificate";
  options[5] = (char *) calloc (PATH_MAX,1);
  snprintf(options[5],PATH_MAX,"/var/lib/shim/ssl_cert.pem");
  options[6] = "authentication_domain";
  options[7] = "";
  options[8] = NULL;
  TMPDIR = DEFAULT_TMPDIR;
  TIMEOUT = DEFAULT_TIMEOUT;
  MAX_SESSIONS = DEFAULT_MAX_SESSIONS;
  SAVE_INSTANCE_ID = DEFAULT_SAVE_INSTANCE_ID;
  counter = 19;

/* Set up a default token for digest authentication.  */
  default_token.val = 1;
  default_token.time = 0;
  default_token.uid = getuid();
  default_token.next = NULL;

  parse_args (options, argc, argv, &daemonize);
  if (stat (options[5], &check_ssl) < 0)
    {
/* Disable SSL  by removing any 's' port options and getting rid of the ssl
 * options.
 */
      syslog (LOG_ERR, "Disabling SSL, error reading %s", options[5]);
      ports = cp = strdup (options[1]);
      while ((cp = strchr (cp, 's')) != NULL)
        *cp++ = ',';
      options[1] = ports;
      options[4] = NULL;
      free(options[5]);
      options[5] = NULL;
    }
  docroot = options[3];
  sessions = (session *) calloc (MAX_SESSIONS, sizeof (session));
  memset (&callbacks, 0, sizeof (callbacks));
  real_uid = getuid ();
  signal (SIGTERM, signalHandler);

  BASEPATH = dirname (argv[0]);

/* Daemonize */
  k = -1;
  if (daemonize > 0)
    {
      k = fork ();
      switch (k)
        {
        case -1:
          fprintf (stderr, "fork error: service terminated.\n");
          exit (1);
        case 0:
/* Close all open file descriptors */
          resLimit.rlim_max = 0;
          getrlimit (RLIMIT_NOFILE, &resLimit);
          l = resLimit.rlim_max;
          for (j = 0; j < l; j++)
            (void) close (j);
          j = open ("/dev/null", O_RDWR);
          dup (j);
          dup (j);
          break;
        default:
          exit (0);
        }
    }

/* Write out my PID */
  j = open (PIDFILE, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (j > 0)
    {
      snprintf (pbuf, MAX_VARLEN, "%d            ", (int) getpid ());
      write (j, pbuf, strlen (pbuf));
      close (j);
    }

  openlog ("shim", LOG_CONS | LOG_NDELAY, LOG_USER);
  omp_init_lock (&biglock);
  for (j = 0; j < MAX_SESSIONS; ++j)
    {
      sessions[j].available = SESSION_AVAILABLE;
      sessions[j].sessionid = 0;
      omp_init_lock (&sessions[j].lock);
    }

  callbacks.begin_request = begin_request_handler;
  ctx = mg_start (&callbacks, NULL, (const char **) options);
  if (!ctx)
    {
      syslog (LOG_ERR, "failed to start web service");
      return -1;
    }
  syslog (LOG_INFO,
          "SciDB HTTP service started on port(s) %s with web root [%s], talking to SciDB on port %d",
          mg_get_option (ctx, "listening_ports"),
          mg_get_option (ctx, "document_root"), SCIDB_PORT);

  for (;;)
    sleep (100);
  omp_destroy_lock (&biglock);
  mg_stop (ctx);
  closelog ();
  free(options[5]);

  return 0;
}
