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
#include "mongoose.h"
#include "pam.h"
#include <pwd.h>

#define MAX_SESSIONS 30         // Maximum number of simultaneous http sessions
#define MAX_VARLEN 4096         // Static buffer length to hold http query params
#define LCSV_MAX 16384
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define MAX_RETURN_BYTES 10000000

#ifndef DEFAULT_HTTP_PORT
#define DEFAULT_HTTP_PORT "8080,8083s"
#endif

#define DEFAULT_TMPDIR "/dev/shm/"      // Temporary location for I/O buffers
#define PIDFILE "/var/run/shim.pid"

#define WEEK 604800             // One week in seconds
#define TIMEOUT 60              // Timeout before a session is declared
                                // orphaned and reaped

#define TELEMETRY_ENTRIES 2000  // Max number of telemetry items (circular buf)
#define TELEMETRY_BUFFER_SIZE 128     // Max size of a single line of telemetry
#define TELEMETRY_UPDATE_INTERVAL 10   // In seconds, update client freq.
#define WEBSOCKET_FRAME_SIZE 32768

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
unsigned long long execute_prepared_query (void *, struct prep *, int,
                                           char *);
// End of mimimalist SciDB client API -----------------------------------------

/* A session consists of client I/O buffers, and an optional SciDB query ID. */
int scount;
typedef struct
{
  omp_lock_t lock;
  int sessionid;                // session identifier
  unsigned long long queryid;   // SciDB query identifier
  int pd;                       // output buffer file descrptor
  FILE *pf;                     //   and FILE pointer
  char *ibuf;                   // input buffer name
  char *obuf;                   // output buffer name
  void *con;                    // SciDB context
  time_t time;                  // Time value to help decide on orphan sessions
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
session *sessions;              // Fixed pool of web client sessions
char *docroot;
static uid_t real_uid;          // For setting uid to logged in user when required
char *PAM_service_name = "login";       // Default PAM service name
/* Big giant lock used to serialize many operations: */
omp_lock_t biglock;
char *BASEPATH;
char *TMPDIR;                   // temporary files go here
token_list *tokens = NULL;      // the head of the list

/* Telemetry data */
char **telemetry;
unsigned int telemetry_counter;

/*
 * conn: A mongoose client connection
 * type: response mime type using the mimetype enumeration defined above
 * code: HTML integer response code (200=OK)
 * length: length of data buffer
 * data: character buffer to send
 * Write an HTTP response to client connection, it's OK for length=0 and
 * data=NULL.  This routine generates the http header.
 *
 * Response messages are http 1.0 with OK/ERROR header, content length, data.
 * example:
 * HTTP/1.0 <code> OK\r\n
 * Content-Length: <length>\r\n
 * Content-Type: text/html\r\n\r\n
 * <DATA>
 */
void
respond (struct mg_connection *conn, enum mimetype type, int code, int length,
         char *data)
{
  if (code != 200)              // error
    {
      if (data)                 // error with data payload (always presented as text/html here)
        {
          mg_printf (conn, "HTTP/1.0 %d ERROR\r\n"
                     "Content-Length: %lu\r\n"
                     "Content-Type: text/html\r\n\r\n", code, strlen (data));
          mg_write (conn, data, strlen (data));
        }
      else                      // error without any payload
        mg_printf (conn, "HTTP/1.0 %d ERROR\r\n\r\n", code);
      return;
    }
  switch (type)
    {
    case html:
      mg_printf (conn, "HTTP/1.0 200 OK\r\n"
                 "Content-Length: %d\r\n"
                 "Content-Type: text/html\r\n\r\n", length);
      break;
    case plain:
      mg_printf (conn, "HTTP/1.0 200 OK\r\n"
                 "Content-Length: %d\r\n"
                 "Content-Type: text/plain\r\n\r\n", length);
      break;
    case binary:
      mg_printf (conn, "HTTP/1.0 200 OK\r\n"
                 "Content-Length: %d\r\n"
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
      ans = &sessions[j];
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
  s->sessionid = -1;            // -1 indicates availability
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
      omp_set_lock (&s->lock);
      if (s->con)
        {
// Establish a new SciDB context used to issue the cancel query.
          can_con = scidbconnect (SCIDB_HOST, SCIDB_PORT);
// check for valid context from scidb
          if (!can_con)
            {
              syslog (LOG_ERR,
                      "cancel_query error could not connect to SciDB");
              omp_unset_lock (&s->lock);
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
      omp_unset_lock (&s->lock);
      respond (conn, plain, 200, 0, NULL);
    }
  else
    respond (conn, plain, 404, 0, NULL);        // not found
}

int
init_session (session * s)
{
  int fd;
  omp_set_lock (&s->lock);
  s->ibuf = (char *) malloc (PATH_MAX);
  s->obuf = (char *) malloc (PATH_MAX);
  snprintf (s->ibuf, PATH_MAX, "%s/scidb_input_buf_XXXXXX", TMPDIR);
  snprintf (s->obuf, PATH_MAX, "%s/scidb_output_buf_XXXXXX", TMPDIR);
// Set up the input buffer
  fd = mkstemp (s->ibuf);
// XXX We need to make it so that whoever runs scidb can R/W to this file.
// Since, in general, we don't know who is running scidb (anybody can), and, in
// general, shim runs as a service we have a mismatch. Right now, we set it so
// that anyone can write to these to work around this. But really, these files
// should be owned by the scidb process user. Hmmm.  (See also below for output
// buffer.)
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
  time (&s->time);
  s->sessionid = scount++;
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
  int j, id = -1;
  time_t t;
  omp_set_lock (&biglock);
  for (j = 0; j < MAX_SESSIONS; ++j)
    {
      if (sessions[j].sessionid < 0)
        {
          if (init_session (&sessions[j]) > 0)
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
      syslog (LOG_INFO, "new_session session id=%d ibuf=%s obuf=%s",
              sessions[j].sessionid, sessions[j].ibuf, sessions[j].obuf);
      snprintf (buf, MAX_VARLEN, "%d\r\n", sessions[j].sessionid);
      respond (conn, plain, 200, strlen (buf), buf);
    }
  else
    {
      respond (conn, plain, 503, 0, NULL);      // out of resources
    }
}

/* Experimental: Load an uploaded CSV file with loadcsv
 * NOTE! This is an experimental function and presently limited to
 * secure connections. And the user must have write privilege to
 * the SciDB data directories (the usual loadcsv.py limitation).
 * /loadcsv
 * --Parameters--
 * id:      <session id>
 * auth:   <optional auth token>
 * schema: array schema
 * name:   array name
 * delim:  optional single-character delimiter (default ,)
 * head:   header lines (integer >= 0)
 * nerr:   number of tolerable errors (integer >= 0)
 *
 * And the function must be called with a valid user id for seteuid.
 */
void
loadcsv (struct mg_connection *conn, const struct mg_request_info *ri,
         uid_t uid)
{
  int id, k, n, j;
  session *s;
  char *LD;
  char var[MAX_VARLEN];
  char schema[MAX_VARLEN];
  char buf[MAX_VARLEN];
  char arrayname[MAX_VARLEN];
  char cmd[LCSV_MAX];

  struct passwd pwd;
  struct passwd *result;
  char *pbuf;
  size_t bufsize;

  if (!ri->query_string)
    {
      respond (conn, plain, 400, 0, NULL);
      syslog (LOG_ERR, "loadcsv error invalid http query");
      return;
    }
  syslog (LOG_INFO, "loadcsv querystring %s", ri->query_string);
  k = strlen (ri->query_string);
  mg_get_var (ri->query_string, k, "id", var, MAX_VARLEN);
  id = atoi (var);
  s = find_session (id);
  if (!s)
    {
      syslog (LOG_INFO, "loadcsv session error");
      respond (conn, plain, 404, 0, NULL);
      return;
    }
  omp_set_lock (&s->lock);
// Check to see if the upload buffer is open for reading, if not do so.
  if (s->pd < 1)
    {
      s->pd = open (s->ibuf, O_RDONLY | O_NONBLOCK);
      if (s->pd < 1)
        {
          syslog (LOG_ERR, "loadcsv error opening upload buffer");
          respond (conn, plain, 500, 0, NULL);
          omp_unset_lock (&s->lock);
          return;
        }
    }
// Retrieve the schema
  memset (schema, 0, MAX_VARLEN);
  mg_get_var (ri->query_string, k, "schema", schema, MAX_VARLEN);
// Retrieve the new array name
  memset (arrayname, 0, MAX_VARLEN);
  mg_get_var (ri->query_string, k, "name", arrayname, MAX_VARLEN);
// Retrieve the number of header lines
  memset (var, 0, MAX_VARLEN);
  mg_get_var (ri->query_string, k, "head", var, MAX_VARLEN);
  n = atoi (var);
// Retrieve the number of errors allowed
  memset (var, 0, MAX_VARLEN);
  mg_get_var (ri->query_string, k, "nerr", var, MAX_VARLEN);

  bufsize = sysconf (_SC_GETPW_R_SIZE_MAX);
  if (bufsize == -1)            /* Value was indeterminate */
    bufsize = 16384;            /* Should be enough */
  pbuf = malloc (bufsize);
  if (pbuf == NULL)
    {
      goto bail;
    }
  getpwuid_r (uid, &pwd, pbuf, bufsize, &result);
  if (result == NULL)
    {
      free (pbuf);
      goto bail;
    }
  LD = getenv ("LD_LIBRARY_PATH");
// User access is restricted by seteuid and then su. We add su to allow
// use of LD_LIBRARY_PATH.
  snprintf (cmd, LCSV_MAX,
            "su %s -c \"/bin/bash -c \\\"umask 022; export LD_LIBRARY_PATH=%s ; %s/csv2scidb  -s %d < %s > %s.scidb ; %s/iquery -naq 'remove(%s)' 2>>/tmp/log; %s/iquery -naq 'create_array(%s,%s)' 2>>/tmp/log; %s/iquery -naq \\\\\\\"store(input(%s,'%s.scidb',0),%s)\\\\\\\" 2>>/tmp/log;rm -f %s.scidb\\\" \" ",
            pwd.pw_name, LD, BASEPATH, n, s->ibuf, s->ibuf, BASEPATH,
            arrayname, BASEPATH, arrayname, schema, BASEPATH, schema, s->ibuf,
            arrayname, s->ibuf);
//  snprintf (cmd, LCSV_MAX,
//            "/bin/bash -l -c \"umask 022; export LD_LIBRARY_PATH=%s ; %s/csv2scidb  -s %d < %s > %s.scidb ; %s/iquery -naq 'remove(%s)' 2>>/tmp/log; %s/iquery -naq 'create_array(%s,%s)' 2>>/tmp/log; %s/iquery -naq \\\"store(input(%s,'%s.scidb',0),%s)\\\" 2>>/tmp/log;touch %s.scidb\"",
//            LD, BASEPATH, n, s->ibuf, s->ibuf, BASEPATH, arrayname,  BASEPATH,
//            arrayname, schema, BASEPATH, schema, s->ibuf, arrayname, s->ibuf);
//  snprintf(cmd,LCSV_MAX, "export LD_LIBRARY_PATH=%s && python %s/loadcsv.py -v -m -l -M -L -x -i %s -n %d -e %d -a %s -s \"%s\" >/tmp/log 2>&1",  getenv("LD_LIBRARY_PATH"), BASEPATH, s->ibuf, n, e, arrayname, schema);

  syslog (LOG_INFO, "loadcsv euid: %ld user name %s; cmd: %s", (long) uid,
          pwd.pw_name, cmd);
  free (pbuf);
  j = seteuid (uid);
  if (j < 0)
    {
      goto bail;
    }
  n = system (cmd);
  seteuid (real_uid);
  syslog (LOG_INFO, "loadcsv result: %d", n);
  syslog (LOG_INFO, "loadcsv releasing HTTP session %d", s->sessionid);
  cleanup_session (s);
  omp_unset_lock (&s->lock);
// Respond to the client
  snprintf (buf, MAX_VARLEN, "%d", n);
  respond (conn, plain, 200, strlen (buf), buf);
  return;

bail:
  syslog (LOG_ERR, "Setuid error");
  cleanup_session (s);
  omp_unset_lock (&s->lock);
  respond (conn, plain, 401, strlen ("Not authorized"), "Not authorized");
}


/* Read bytes from a query result output buffer.
 * The mg_request_info must contain the keys:
 * n=<max number of bytes to read -- signed int32>
 * id=<session id>
 * Writes at most n bytes back to the client or a 410 error if something
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
  omp_set_lock (&s->lock);
// Check to see if the output buffer is open for reading, if not do so.
  if (s->pd < 1)
    {
      s->pd = open (s->obuf, O_RDONLY | O_NONBLOCK);
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
      syslog (LOG_INFO, "readbytes returning entire buffer");
      mg_send_file (conn, s->obuf);
      omp_unset_lock (&s->lock);
      return;
    }
  if (n > MAX_RETURN_BYTES)
    n = MAX_RETURN_BYTES;

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
  if (l < 0)                    // This is just EOF
    {
      free (buf);
      respond (conn, plain, 410, 0, NULL);
      omp_unset_lock (&s->lock);
      return;
    }
  respond (conn, binary, 200, l, buf);
  time (&s->time);
  omp_unset_lock (&s->lock);
  free (buf);
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
// Retrieve max number of lines to read
  mg_get_var (ri->query_string, k, "n", var, MAX_VARLEN);
  n = atoi (var);
// Check to see if client wants the whole file at once, if so return it.
  omp_set_lock (&s->lock);
  if (n < 1)
    {
      syslog (LOG_INFO, "readlines returning entire buffer");
      mg_send_file (conn, s->obuf);
      omp_unset_lock (&s->lock);
      return;
    }
// Check to see if output buffer is open for reading
  syslog (LOG_INFO, "readlines opening buffer");
  if (s->pd < 1)
    {
      s->pd = open (s->obuf, O_RDONLY | O_NONBLOCK);
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

/* execute_query blocks until the query is complete.
 * This function always disconnects from SciDB after completeQuery finishes.
 * query string variables:
 * id=<session id> (required)
 * query=<query string> (required)
 * release={0 or 1}  (optional, default 0)
 *   release > 0 invokes release_session after completeQuery.
 * save=<format string> (optional, default 0-length string)
 *   set the save format to something to wrap the query in a save
 * to the session pipe.
 *
 * Any error that occurs during execute_query that is associated
 * with a valid session ID results in the release of the session.
 */
void
execute_query (struct mg_connection *conn, const struct mg_request_info *ri)
{
  int id, k, rel = 0, async = 0;
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
  mg_get_var (ri->query_string, k, "async", var, MAX_VARLEN);
  if (strlen (var) > 0)
    async = atoi (var);
  if (rel == 0)
    async = 0;
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
// XXX Experimental async flag, if set respond immediately
  if (async)
    {
      syslog (LOG_INFO, "execute_query async indicated");
      respond (conn, plain, 200, 0, NULL);
    }
  omp_set_lock (&s->lock);
  memset (var, 0, MAX_VARLEN);
  mg_get_var (ri->query_string, k, "save", save, MAX_VARLEN);
  mg_get_var (ri->query_string, k, "query", qrybuf, k);
// If save is indicated, modify query
  if (strlen (save) > 0)
    snprintf (qry, k + MAX_VARLEN, "save(%s,'%s',0,'%s')", qrybuf, s->obuf,
              save);
  else
    snprintf (qry, k + MAX_VARLEN, "%s", qrybuf);

  if (!s->con)
    s->con = scidbconnect (SCIDB_HOST, SCIDB_PORT);
  syslog (LOG_INFO, "execute_query s->con = %p %s", s->con, qry);
  if (!s->con)
    {
      free (qry);
      free (qrybuf);
      syslog (LOG_ERR, "execute_query error could not connect to SciDB");
      if (!async)
        respond (conn, plain, 503, strlen ("Could not connect to SciDB"),
                 "Could not connect to SciDB");
      cleanup_session (s);
      omp_unset_lock (&s->lock);
      return;
    }

  syslog (LOG_INFO, "execute_query connected");
  prepare_query (&pq, s->con, qry, 1, SERR);
  l = pq.queryid;
  syslog (LOG_INFO, "execute_query scidb queryid = %llu", l);
  if (l < 1 || !pq.queryresult)
    {
      free (qry);
      free (qrybuf);
      syslog (LOG_ERR, "execute_query error %s", SERR);
      if (!async)
        respond (conn, plain, 500, strlen (SERR), SERR);
      if (s->con)
        scidbdisconnect (s->con);
      s->con = NULL;
      cleanup_session (s);
      omp_unset_lock (&s->lock);
      return;
    }
/* Set the queryID for potential future cancel event
 * The time flag is set to a future value to prevent get_session from
 * declaring this session orphaned while a query is running. This
 * session cannot be reclaimed until the query finishes, since the
 * lock is held.
 */
  s->queryid = l;
  s->time = time (NULL) + WEEK;
  if (s->con)
    l = execute_prepared_query (s->con, &pq, 1, SERR);
  if (l < 1)
    {
      free (qry);
      free (qrybuf);
      syslog (LOG_ERR, "execute_prepared_query error %s", SERR);
      if (!async)
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
  snprintf (buf, MAX_VARLEN, "%llu", l);        // Return the query ID
  if (!async)
    respond (conn, plain, 200, strlen (buf), buf);
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

/* Telemetry and log callbacks
 * - measurement uploads new telemetry data
 * - webocket_ready_handler periodically sends telemetry data to an open
 *   client websocket connection, it's a mongoose callback function that
 *   gets instantiated as a thread for each websocket connection.
 */
void
measurement (struct mg_connection *conn, const struct mg_request_info *ri)
{
  int k;
  char var[TELEMETRY_BUFFER_SIZE];
  time_t t;
  if (!ri->query_string)
    {
      respond (conn, plain, 400, 0, NULL);
      syslog (LOG_ERR, "measurement error invalid http query");
      return;
    }
  k = strlen (ri->query_string);
  mg_get_var (ri->query_string, k, "data", var, TELEMETRY_BUFFER_SIZE);
  omp_set_lock (&biglock);
  time (&t);
// Add current server time in seconds to the measurement
  snprintf (telemetry[telemetry_counter], TELEMETRY_BUFFER_SIZE, "%ld,%s",
            (long int) t, var);
  telemetry_counter = (telemetry_counter + 1) % TELEMETRY_ENTRIES;
  omp_unset_lock (&biglock);
  respond (conn, plain, 200, 0, NULL);
}

static void
websocket_ready_handler (struct mg_connection *conn)
{
  unsigned char *buf;           // websocket response method
  unsigned char *p;             // generic pointer into buf
  unsigned char x;
  int16_t l;                    // Length of the websocket payload
  int k, h;
  unsigned int j;
// tracks the last line that this thread has sent...
  unsigned int counter = TELEMETRY_ENTRIES + 1;
// The websocket header takes 4 bytes in this case.
  buf = (unsigned char *) malloc (WEBSOCKET_FRAME_SIZE);
  buf[1] = 126;                 // -> two-byte length in buf[2], buf[3].

  for (;;)
    {
      buf[0] = 0x81;            // FIN + TEXT (aka a single utf8 text message)
      p = buf + 4;
      l = 0;
      if (counter == telemetry_counter)
        {
// Nothing new to send.
          goto skip;
        }
      else if (counter > TELEMETRY_ENTRIES)
        {
// First message, send everything we got. If the telemetry buffer
// exceeds the websocket frame limit, we send this as sequence of fragmented
// frames. It's a bit trcky.
          buf[0] = 1;           // FIN=0 TEXT (fragmented text message)
          j = 0;
          while (j < TELEMETRY_ENTRIES)
            {
              k = strlen (telemetry[j]) + 1;    // + newline
              if ((k + (p - buf) + 2) > WEBSOCKET_FRAME_SIZE)   // + 2 = trailing string null + safety margin
                {
// The message exceeds a single frame. Send a fragmented frame.
                  l = (p - buf) - 4;
                  memcpy ((void *) buf + 2, (const void *) &l, sizeof (l));
// poor man's htons
                  x = buf[3];
                  buf[3] = buf[2];
                  buf[2] = x;
                  h = mg_write (conn, buf, l + 4);
                  syslog (LOG_INFO, "sent %d-byte fragmented frame op 0x%x",
                          h, buf[0]);
                  buf[0] = 0;   // Next frame is a continuation frame
                  p = buf + 4;  // reset the buffer pointer
                }
              snprintf ((char *restrict) p, TELEMETRY_BUFFER_SIZE, "%s\n",
                        telemetry[j]);
              p = p + k;
// Note! We intentionally do not include the string's trailing zero byte.
              j++;
            }
          if (buf[0] == 1)
            {
              buf[0] = 0x81;    // only one frame
//syslog(LOG_INFO, "SINGLE FRAME");
            }
          else
            {
              buf[0] = 128;     // next frame is final
//syslog(LOG_INFO,"FINAL CONTINUATION FRAME");
            }

        }
      else if (counter < telemetry_counter)
        {
          for (j = counter; j < telemetry_counter; ++j)
            {
              k = strlen (telemetry[j]) + 1;    // newline
              if (p + k - buf < WEBSOCKET_FRAME_SIZE)
                {
                  snprintf ((char *restrict) p, TELEMETRY_BUFFER_SIZE, "%s\n",
                            telemetry[j]);
// Note! We intentionally do not include the string's trailing zero byte.
                  p += k;
                }
            }
        }
      else
        {
          for (j = counter; j < TELEMETRY_ENTRIES; ++j)
            {
              k = strlen (telemetry[j]) + 1;    // newline
              if (p + k - buf < WEBSOCKET_FRAME_SIZE)
                {
                  snprintf ((char *restrict) p, TELEMETRY_BUFFER_SIZE, "%s\n",
                            telemetry[j]);
                  p += k;
                }
            }
          for (j = 0; j < telemetry_counter; ++j)
            {
              k = strlen (telemetry[j]) + 1;    // newline
              if (p + k - buf < WEBSOCKET_FRAME_SIZE)
                {
                  snprintf ((char *restrict) p, TELEMETRY_BUFFER_SIZE, "%s\n",
                            telemetry[j]);
                  p += k;
                }
            }
        }
      counter = telemetry_counter;
      l = (p - buf) - 4;
// Make sure length of websocket message is in [126,32768) for websocket framing
// reasons. If it is too small, pad it with newlines. The client needs to
// expect empty newlines on occasion and deal with them.
      if (l < 127)
        {
          memset (p, 10, 127);  // Pad out with newlines
          memset (p + 127, 0, 1);
          p += 128;
        }
      l = (p - buf) - 4;
      memcpy ((void *) buf + 2, (const void *) &l, sizeof (l));
// poor man's htons
      x = buf[3];
      buf[3] = buf[2];
      buf[2] = x;
      k = mg_write (conn, buf, l + 4);
      syslog (LOG_INFO, "sent %d telemetry bytes op 0x%x", k, buf[0]);
      if (k < 1)
        {
// The client connection must have closed, break out of loop and terminate
// this thread.
          syslog (LOG_INFO, "websocket connection closed");
          break;
        }
    skip:
      sleep (TELEMETRY_UPDATE_INTERVAL);
    }
  free (buf);
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
  if (!ri->query_string)
    {
      respond (conn, plain, 400, 0, NULL);
      syslog (LOG_ERR, "authentication error invalid http query");
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
 * the listed subset of the available shim API)
 */
  if (ri->is_ssl &&
      (!strcmp (ri->uri, "/new_session") ||
       !strcmp (ri->uri, "/upload_file") ||
       !strcmp (ri->uri, "/read_lines") ||
       !strcmp (ri->uri, "/read_bytes") ||
       !strcmp (ri->uri, "/execute_query") ||
       !strcmp (ri->uri, "/loadcsv") ||
       !strcmp (ri->uri, "/cancel")) &&
      !(tok = check_auth (tokens, conn, ri)))
    goto end;

// CLIENT API
  if (!strcmp (ri->uri, "/new_session"))
    new_session (conn);
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
  else if (!strcmp (ri->uri, "/loadcsv"))
  {
    if(!tok)
    {
      syslog (LOG_ERR, "loadcsv not authorized");
      respond (conn, plain, 401, strlen("Not authorized"), "Not authorized");
      goto end;
    }
    loadcsv (conn, ri, tok->uid);
  }
  else if (!strcmp (ri->uri, "/cancel"))
    cancel_query (conn, ri);
// CONTROL API
//      else if (!strcmp (ri->uri, "/stop_scidb"))
//        stopscidb (conn, ri);
//      else if (!strcmp (ri->uri, "/start_scidb"))
//        startscidb (conn, ri);
  else if (!strcmp (ri->uri, "/get_log"))
    getlog (conn, ri);
/* Telemetry API */
  else if (!strcmp (ri->uri, "/measurement"))
    measurement (conn, ri);
  else if (!strcmp (ri->uri, "/telemetry"))
    return 0;
  else
    {
// fallback to http file server
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
  while ((c = getopt (argc, argv, "hfn:p:r:s:t:")) != -1)
    {
      switch (c)
        {
        case 'h':
          printf
            ("Usage:\nshim [-h] [-f] [-n <PAM service name>] [-p <http port>] [-r <document root>] [-s <scidb port>] [-t <tmp I/O DIR>]\n");
          printf
            ("Specify -f to run in the foreground.\nDefault http ports are 8080 and 80803(SSL).\nDefault SciDB port is 1239.\nDefault document root is /var/lib/shim/wwwroot.\nDefault PAM service name is 'login'.\n");
          printf
            ("Start up shim and view http://localhost:8080/api.html from a browser for help with the API.\n\n");
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
          options[5] = (char *) malloc (PATH_MAX);
          strncat (options[5], optarg, PATH_MAX);
          strncat (options[5], "/../ssl_cert.pem", PATH_MAX - 17);
          break;
        case 's':
          SCIDB_PORT = atoi (optarg);
          break;
        case 't':
          TMPDIR = optarg;
          break;
        default:
          break;
        }
    }
}





/* Usage:
 * shim [-h] [-f] [-p port] [-r path] [-s scidb_port]
 * See mongoose manual for specifying multiple http ports.
 */
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
  char *options[7];
  options[0] = "listening_ports";
  options[1] = DEFAULT_HTTP_PORT;
  options[2] = "document_root";
  options[3] = "/var/lib/shim/wwwroot";
  options[4] = "ssl_certificate";
  options[5] = "/var/lib/shim/ssl_cert.pem";
  options[6] = NULL;
  TMPDIR = DEFAULT_TMPDIR;
  parse_args (options, argc, argv, &daemonize);
  if (stat (options[5], &check_ssl) < 0)
    {
/* Disable SSL  by removing any 's' port options and getting rid of the ssl
 * options.
 */
      ports = cp = strdup (options[1]);
      while ((cp = strchr (cp, 's')) != NULL)
        *cp++ = ',';
      options[1] = ports;
      options[4] = NULL;
      options[5] = NULL;
    }
  docroot = options[3];
  sessions = (session *) calloc (MAX_SESSIONS, sizeof (session));
  scount = 0;
  memset (&callbacks, 0, sizeof (callbacks));
  real_uid = getuid ();

/* Set up telemetry storage. It's a fixed buffer. */
  telemetry = (char **) malloc (TELEMETRY_ENTRIES * sizeof (char *));
  for (k = 0; k < TELEMETRY_ENTRIES; ++k)
    {
      telemetry[k] = (char *) calloc (TELEMETRY_BUFFER_SIZE, sizeof (char));
    }
  telemetry_counter = 0;

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
      sessions[j].sessionid = -1;
      omp_init_lock (&sessions[j].lock);
    }

  callbacks.begin_request = begin_request_handler;
  callbacks.websocket_ready = websocket_ready_handler;
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

  return 0;
}
