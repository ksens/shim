#define _GNU_SOURCE
#include <stdio.h>
#include <limits.h>
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

// Should we make the TMPDIR a runtime option (probably yes)?
#ifndef TMPDIR
#define TMPDIR "/tmp"           // Temporary location for I/O buffers
#endif
#define PIDFILE "/var/run/shim.pid"

#define WEEK 604800             // One week in seconds
#define TIMEOUT 60              // Timeout before a session is declared orphaned and reaped

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

session *sessions;              // Fixed pool of web client sessions
char *docroot;
char SCIDB_HOST[] = "localhost";
int SCIDB_PORT = 1239;
omp_lock_t biglock;
char *BASEPATH;


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

/* Authenticate
 * POST username and password XXX
 * XXX in process
 */
void
auth (struct mg_connection *conn, const struct mg_request_info *ri)
{
  int k;
  char u[MAX_VARLEN];
  char p[MAX_VARLEN];
  if (!ri->query_string || !ri->is_ssl)
    {
      respond (conn, plain, 400, 0, NULL);
      syslog (LOG_INFO, "auth error invalid http query");
      return;
    }
  k = strlen (ri->query_string);
  mg_get_var (ri->query_string, k, "username", u, MAX_VARLEN);
  mg_get_var (ri->query_string, k, "password", p, MAX_VARLEN);
  k = do_pam_login("login",u,p);
// XXX XXX If successful, enroll user and return an auth token.
  if(k==0)
    respond (conn, plain, 200, strlen ("HOMER\n"), "HOMER\n");
  else
    respond (conn, plain, 401, 0, NULL);
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

/* Load an uploaded CSV file with loadcsv */
void
loadcsv (struct mg_connection *conn, const struct mg_request_info *ri)
{
  int id, k, n;
  session *s;
  char var[MAX_VARLEN];
  char schema[MAX_VARLEN];
  char buf[MAX_VARLEN];
  char arrayname[MAX_VARLEN];
  char cmd[LCSV_MAX];
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
  mg_get_var (ri->query_string, k, "err", var, MAX_VARLEN);
  snprintf (cmd, LCSV_MAX,
            "%s/csv2scidb  -s %d < %s > %s.scidb; %s/iquery -naq 'remove(%s)'; %s/iquery -naq 'create_array(%s,%s)'; %s/iquery -naq \"store(input(%s,'%s.scidb',0),%s)\";rm -f %s.scidb",
            BASEPATH, n, s->ibuf, s->ibuf, BASEPATH, arrayname, BASEPATH,
            arrayname, schema, BASEPATH, schema, s->ibuf, arrayname, s->ibuf);
// It's a bummer, but I can't get loadcsv.py to work!
//  snprintf(cmd,LCSV_MAX, "%s/loadcsv.py -i %s -n %d -e %d -a %s -s \"%s\"", BASEPATH, s->ibuf, n, e, arrayname, schema);
  syslog (LOG_INFO, "loadcsv cmd: %s", cmd);
  n = system (cmd);
  syslog (LOG_INFO, "loadcsv result: %d", n);
  syslog (LOG_INFO, "loadcsv releasing HTTP session %d", s->sessionid);
  cleanup_session (s);
  omp_unset_lock (&s->lock);
// Respond to the client
  snprintf (buf, MAX_VARLEN, "%d", n);
  respond (conn, plain, 200, strlen (buf), buf);
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
  int id, k, rel = 0;
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
  qrybuf = (char *) malloc (k);
  if (!qrybuf)
    {
      syslog (LOG_ERR, "execute_query error out of memory");
      respond (conn, plain, 404, strlen ("Out of memory"), "Out of memory");
      return;
    }
  qry = (char *) malloc (k + MAX_VARLEN);
  if (!qry)
    {
      free (qrybuf);
      syslog (LOG_ERR, "execute_query error out of memory");
      respond (conn, plain, 404, strlen ("Out of memory"), "Out of memory");
      return;
    }
  mg_get_var (ri->query_string, k, "id", var, MAX_VARLEN);
  id = atoi (var);
  memset (var, 0, MAX_VARLEN);
  mg_get_var (ri->query_string, k, "release", var, MAX_VARLEN);
  if (strlen (var) > 0)
    rel = atoi (var);
  syslog (LOG_INFO, "execute_query for session id %d", id);
  s = find_session (id);
  if (!s)
    {
      free (qrybuf);
      free (qry);
      syslog (LOG_ERR, "execute_query error Invalid session ID %d", id);
      respond (conn, plain, 404, strlen ("Invalid session ID"),
               "Invalid session ID");
      return;                   // check for valid session
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

void
getlog (struct mg_connection *conn, const struct mg_request_info *ri)
{
  syslog (LOG_ERR, "getlog");
  system
    ("cp `ps axu | grep SciDB | grep \"\\/000\\/0\"  | grep SciDB | head -n 1 | sed -e \"s/SciDB-000.*//\" | sed -e \"s/.* \\//\\//\"`/scidb.log /tmp/.scidb.log");
  mg_send_file (conn, "/tmp/.scidb.log");
}


/* Mongoose generic begin_request callback; we dispatch URIs to their
 * appropriate handlers.
 */
static int
begin_request_handler (struct mg_connection *conn)
{
  char buf[MAX_VARLEN];
  const struct mg_request_info *ri = mg_get_request_info (conn);

  if(ri->is_ssl && !strcmp (ri->uri, "/auth"))
    syslog (LOG_INFO, "(SSL)callback for %s", ri->uri);
  else if(ri->is_ssl)
    syslog (LOG_INFO, "(SSL)callback for %s%s", ri->uri, ri->query_string);
  else
    syslog (LOG_INFO, "callback for %s%s", ri->uri, ri->query_string);
// CLIENT API
  if (!strcmp (ri->uri, "/new_session"))
    new_session (conn);
  else if (!strcmp (ri->uri, "/auth"))
    auth (conn, ri);
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
    loadcsv (conn, ri);
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
      if (!strcmp (ri->uri, "/"))
        snprintf (buf, MAX_VARLEN, "%s/index.html", docroot);
      else
        snprintf (buf, MAX_VARLEN, "%s/%s", docroot, ri->uri);
      mg_send_file (conn, buf);
    }

// Mark as processed by returning non-null value.
  return 1;
}


/* Parse the command line options, updating the options array */
void
parse_args (char **options, int argc, char **argv, int *daemonize)
{
  int c;
  while ((c = getopt (argc, argv, "hfp:r:s:")) != -1)
    {
      switch (c)
        {
        case 'h':
          printf
            ("Usage:\nshim [-h] [-f] [-p <http port>] [-r <document root>] [-s <scidb port>]\n");
          printf
            ("Specify -f to run in the foreground.\nDefault http port is 8080.\nDefault SciDB port is 1239.\nDefault document root is /var/lib/shim/wwwroot.\n");
          printf
            ("Start up shim and view http://localhost:8080/api.html from a browser for help with the API.\n\n");
          exit (0);
          break;
        case 'f':
          *daemonize = 0;
          break;
        case 'p':
          options[1] = optarg;
          break;
        case 'r':
          options[3] = optarg;
          break;
        case 's':
          SCIDB_PORT = atoi (optarg);
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
  parse_args (options, argc, argv, &daemonize);
  if(stat("/var/lib/shim/ssl_cert.pem", &check_ssl) < 0)
  {
/* Disable TLS by removing any 's' port options and getting rid of the ssl
 * options.
 */
    ports = cp = strdup(options[1]);
    while((cp = strchr(cp, 's')) != NULL) *cp++ = ',';
    options[1] = ports;
    options[4] = NULL;
    options[5] = NULL;
  }
  docroot = options[3];
  sessions = (session *) calloc (MAX_SESSIONS, sizeof (session));
  scount = 0;
  memset (&callbacks, 0, sizeof (callbacks));

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
  ctx = mg_start (&callbacks, NULL, (const char **) options);
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
