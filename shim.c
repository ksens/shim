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
#include <omp.h>
#include "mongoose.h"

#define MAX_SESSIONS 20         // Maximum number of simultaneous http sessions
#define MAX_VARLEN 4096         // Static buffer length to hold http query params
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define MAX_RETURN_BYTES 10000000

#ifndef DEFAULT_HTTP_PORT
#define DEFAULT_HTTP_PORT "8080"
#endif

#ifndef TMPDIR
#define TMPDIR "/tmp"           // Temporary location for I/O buffers
#endif

#define DEBUG
//#undef DEBUG

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

/* A session consists of client I/O buffers, and an optional SciDB
 * query ID.
 */
typedef struct
{
  int sessionid;                // session identifier
  unsigned long long queryid;   // <1 indicates no query
  int pd;                       // output buffer file descrptor
  FILE *pf;                     //   and FILE pointer
  char *ibuf;                   // input buffer name
  char *obuf;                   // output buffer name
  void *con;                    // SciDB context
} session;

enum mimetype
{ html, plain, binary };

session *sessions;              // Fixed pool of web client sessions
omp_lock_t lock;                // Lock for sessions pool
char *fdir;
char *cfgini;
char SCIDB_HOST[] = "localhost";
int SCIDB_PORT = 1239;


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
  omp_set_lock (&lock);
  for (j = 0; j < MAX_SESSIONS; ++j)
    {
      if (sessions[j].sessionid != id)
        continue;
      ans = &sessions[j];
    }
  omp_unset_lock (&lock);
  return ans;
}

/* Cleanup a shim session and reset it to available.
 * Acquire the lock before invoking this routine!
 */
void
cleanup_session (session * s)
{
  syslog (LOG_INFO, "cleanup_session releasing %d", s->sessionid);
  s->sessionid = -1;            // -1 indicates availability
  s->queryid = 0;
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
      syslog (LOG_INFO, "cleanup_sessn unlinking %s", s->obuf);
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
      syslog (LOG_ERR, "readbytes error invalid http query");
      return;
    }
  k = strlen (ri->query_string);
  mg_get_var (ri->query_string, k, "id", var1, MAX_VARLEN);
  id = atoi (var1);
  session *s = find_session (id);
  if (s)
    {
      omp_set_lock (&lock);
      cleanup_session (s);
      omp_unset_lock (&lock);
      if (resp)
        respond (conn, plain, 200, 0, NULL);
    }
  else if (resp)
    respond (conn, plain, 404, 0, NULL);        // not found
}

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
      omp_set_lock (&lock);
      if (s->con)
        {
// Establish a new SciDB context used to issue the cancel query.
          can_con = scidbconnect (SCIDB_HOST, SCIDB_PORT);
// check for valid context from scidb
          if (!can_con)
            {
              syslog (LOG_ERR,
                      "cancel_query error could not connect to SciDB");
              omp_unset_lock (&lock);
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
      omp_unset_lock (&lock);
      respond (conn, plain, 200, 0, NULL);
    }
  else
    respond (conn, plain, 404, 0, NULL);        // not found
}

/* Find an available session.  If no sessions are available, return -1.
 * Otherwise, initialize I/O buffers and return the session array index.
 */
int
get_session ()
{
  int j, fd, id = -1;
  omp_set_lock (&lock);
  for (j = 0; j < MAX_SESSIONS; ++j)
    {
      if (sessions[j].sessionid < 0)
        {
          sessions[j].ibuf = (char *) malloc (PATH_MAX);
          sessions[j].obuf = (char *) malloc (PATH_MAX);
          snprintf (sessions[j].ibuf, PATH_MAX, "%s/scidb_input_buf_XXXXXX",
                    fdir);
          snprintf (sessions[j].obuf, PATH_MAX, "%s/scidb_output_buf_XXXXXX",
                    fdir);
// Set up the input buffer
          fd = mkstemp (sessions[j].ibuf);
          chmod (sessions[j].ibuf, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
          if (fd > 0)
            close (fd);
          else
            {
              cleanup_session (&sessions[j]);
              break;
            }
// Set up the output buffer
          sessions[j].pd = 0;
          fd = mkstemp (sessions[j].obuf);
          if (fd > 0)
            close (fd);
          else
            {
              cleanup_session (&sessions[j]);
              break;
            }
// OK to go, assign an ID and return this slot.
          id = j;
          sessions[j].sessionid = rand ();
          break;
        }
    }
  omp_unset_lock (&lock);
  return id;
}

/* Client file upload
 * POST upload to server-side file defined in the session identified
 * by the 'id' variable in the mg_request_info query string.
 * Respond to the client connection as follows:
 * 200 success, <uploaded filename>\n\n returned in body
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
      syslog (LOG_INFO, "readbytes error invalid http query");
      return;
    }
  k = strlen (ri->query_string);
  mg_get_var (ri->query_string, k, "id", var1, MAX_VARLEN);
  id = atoi (var1);
  s = find_session (id);
  if (s)
    {
      mg_append (conn, s->ibuf);
      snprintf (buf, MAX_VARLEN, "%s\r\n", s->ibuf);
// XXX if mg_append fails, report server error too
      respond (conn, plain, 200, strlen (buf), buf);    // XXX report bytes uploaded
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
// Check to see if the output buffer is open for reading, if not do so.
  if (s->pd < 1)
    {
      omp_set_lock (&lock);
      s->pd = open (s->obuf, O_RDONLY | O_NONBLOCK);
      omp_unset_lock (&lock);
      if (s->pd < 1)
        {
          syslog (LOG_ERR, "readbytes error opening output buffer");
          respond (conn, plain, 500, 0, NULL);
          return;
        }
    }
  memset (var, 0, MAX_VARLEN);
// Retrieve max number of bytes to read
  mg_get_var (ri->query_string, k, "n", var, MAX_VARLEN);
  n = atoi (var);
  if (n < 0)
    n = MAX_VARLEN;
  if (n > MAX_RETURN_BYTES)
    n = MAX_RETURN_BYTES;

  buf = (char *) malloc (n);
  if (!buf)
    {
      respond (conn, plain, 507, 0, NULL);
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
  if (l < 0)
    {
      free (buf);
      respond (conn, plain, 410, 0, NULL);
      return;
    }
  respond (conn, binary, 200, l, buf);
  free (buf);
}



/* Read ascii lines from a query result on the query pipe.
 * The mg_request_info must contain the keys:
 * n=<max number of lines to read>
 * id=<session id>
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
// Check to see if output buffer is open for reading
  syslog (LOG_ERR, "readlines opening buffer");
  if (s->pd < 1)
    {
      omp_set_lock (&lock);
      s->pd = open (s->obuf, O_RDONLY | O_NONBLOCK);
      if (s->pd > 0)
        s->pf = fdopen (s->pd, "r");
      omp_unset_lock (&lock);
      if (s->pd < 1 || !s->pf)
        {
          respond (conn, plain, 500, 0, NULL);
          syslog (LOG_ERR, "readlines error opening output buffer");
          return;
        }
    }
  memset (var, 0, MAX_VARLEN);
// Retrieve max number of lines to read
  mg_get_var (ri->query_string, k, "n", var, MAX_VARLEN);
  n = atoi (var);
  if (n < 1)
    {
      respond (conn, plain, 414, strlen ("Invalid n"), "Invalid n");
      syslog (LOG_ERR, "readlines invalid n");
      return;
    }

  k = 0;
  t = 0;                        // Total bytes read so far
  m = MAX_VARLEN;
  v = m * n;                    // Output buffer size
  lbuf = (char *) malloc (m);
  if(!lbuf)
  {
    respond (conn, plain, 500, strlen("Out of memory"), "Out of memory");
    syslog (LOG_ERR, "readlines out of memory");
    return;
  }
  buf = (char *) malloc (v);
  if(!buf)
  {
    free(lbuf);
    respond (conn, plain, 500, strlen("Out of memory"), "Out of memory");
    syslog (LOG_ERR, "readlines out of memory");
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
          if(!tmp)
          {
            free(lbuf);
            free(buf);
            respond (conn, plain, 500, strlen("Out of memory"), "Out of memory");
            syslog (LOG_ERR, "readlines out of memory");
            return;
          } else
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
  char qry[3 * MAX_VARLEN];
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
  syslog (LOG_INFO, "execute_query for session id %d", id);
  s = find_session(id);
  if (!s)
    {
      syslog (LOG_ERR, "execute_query error Invalid session ID %d", id);
      respond (conn, plain, 404, strlen ("Invalid session ID"),
               "Invalid session ID");
      return;                   // check for valid session
    }
  memset (var, 0, MAX_VARLEN);
  mg_get_var (ri->query_string, k, "save", save, MAX_VARLEN);
  mg_get_var (ri->query_string, k, "query", var, MAX_VARLEN);
// If save is indicated, modify query
  if (strlen (save) > 0)
    snprintf (qry, 3 * MAX_VARLEN, "save(%s,'%s',0,'%s')", var, s->obuf,
              save);
  else
    snprintf (qry, 3 * MAX_VARLEN, "%s", var);

  if (!s->con)
    s->con = scidbconnect (SCIDB_HOST, SCIDB_PORT);
  syslog (LOG_INFO, "execute_query s->con = %p %s", s->con, qry);
  if (!s->con)
    {
      syslog (LOG_ERR, "execute_query error could not connect to SciDB");
      respond (conn, plain, 503, strlen ("Could not connect to SciDB"),
               "Could not connect to SciDB");
      omp_set_lock (&lock);
      cleanup_session (s);
      omp_unset_lock (&lock);
      return;
    }

  syslog (LOG_INFO, "execute_query connected");
  prepare_query (&pq, s->con, qry, 1, SERR);
  l = pq.queryid;
  syslog (LOG_INFO, "execute_query scidb queryid = %llu", l);
  if (l < 1 || !pq.queryresult)
    {
      syslog (LOG_ERR, "execute_query error %s", SERR);
      respond (conn, plain, 500, strlen (SERR), SERR);
      omp_set_lock (&lock);
      if (s->con)
        scidbdisconnect (s->con);
      s->con = NULL;
      cleanup_session (s);
      omp_unset_lock (&lock);
      return;
    }
// Set the queryID for potential future cancel event
  omp_set_lock (&lock);
  s->queryid = l;
  omp_unset_lock (&lock);
  if (s->con)
    l = execute_prepared_query (s->con, &pq, 1, SERR);
  if (l < 1)
    {
      syslog (LOG_ERR, "execute_prepared_query error %s", SERR);
      respond (conn, plain, 500, strlen (SERR), SERR);
      omp_set_lock (&lock);
      if (s->con)
        scidbdisconnect (s->con);
      s->con = NULL;
      cleanup_session (s);
      omp_unset_lock (&lock);
      return;
    }
  if (s->con)
    completeQuery (l, s->con, SERR);

  syslog (LOG_INFO, "execute_query %d done, disconnecting", s->sessionid);
  omp_set_lock (&lock);
  if (s->con)
    scidbdisconnect (s->con);
  s->con = NULL;
  if (rel > 0)
    {
      syslog (LOG_INFO, "execute_query releasing HTTP session %d",
              s->sessionid);
      cleanup_session (s);
    }
  omp_unset_lock (&lock);
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
  syslog (LOG_INFO, "stopscidb %s",var);
  snprintf(cmd, 2*MAX_VARLEN, "scidb.py stopall %s", var);
  k = system(cmd);
  respond(conn, plain, 200, 0, NULL);
}

void
startscidb (struct mg_connection *conn, const struct mg_request_info *ri)
{
  int k;
  char var[MAX_VARLEN];
  char cmd[2 * MAX_VARLEN];
  k = strlen (ri->query_string);
  mg_get_var (ri->query_string, k, "db", var, MAX_VARLEN);
  syslog (LOG_INFO, "startscidb %s",var);
  snprintf(cmd, 2*MAX_VARLEN, "scidb.py startall %s", var);
  k = system(cmd);
  respond(conn, plain, 200, 0, NULL);
}

/* Return part of the log of the connected SciDB server.
 * We run a query to locate the log.
 */
void
getlog (struct mg_connection *conn, const struct mg_request_info *ri)
{
  int id;
  FILE *fp;
  unsigned long long l;
  char ERR[MAX_VARLEN];
  char qry[2*MAX_VARLEN];
  session *s;
  ssize_t rd;
  char *x, *line1, *line = NULL;
  size_t n;
  size_t len = 0;
  id = get_session();
  if(id<0)
  {
    syslog (LOG_ERR, "getlog out of resources");
    respond(conn, plain, 503, 0, NULL);
    return;
  }
  s = &sessions[id];
  syslog (LOG_INFO, "getlog session=%d", s->sessionid);
  snprintf(qry, 2*MAX_VARLEN, "save(between(project(list('instances'),instance_path),0,0),'%s',0,'csv')", s->obuf);
  syslog (LOG_INFO, "getlog query=%s", qry);
  s->con = scidbconnect (SCIDB_HOST, SCIDB_PORT);
  l = executeQuery (s->con, qry, 1, ERR);
  syslog (LOG_INFO, "getlog l=%llu", l);
  if(l<1) goto bail;
  fp = fopen(s->obuf, "r");
  while((rd = getline(&line, &len, fp)) != -1) {}
  fclose(fp);
  if(strlen(line)<1)
  {
    respond(conn,plain,404,0,NULL);
  } else
  {
    line[strlen(line)]='\0';
    n = strlen(line) + strlen("/scidb.log");
    line1 = (char *)calloc(n,0);
    x = line + 1;
    strncpy(line1, x, strlen(line)-3);
    strcat(line1, "/scidb.log");
    syslog (LOG_INFO, "getlog sending log %s", line1);
    mg_send_file (conn, line1);
  }
  free(line);

bail:
  omp_set_lock (&lock);
  cleanup_session (s);
  omp_unset_lock (&lock);
}


/* Mongoose callbacks; we dispatch URIs to their appropriate
 * handlers.
 */
static void *
callback (enum mg_event event, struct mg_connection *conn)
{
  char buf[MAX_VARLEN];
  const struct mg_request_info *ri = mg_get_request_info (conn);

  if (event == MG_NEW_REQUEST)
    {
      syslog (LOG_INFO, "callback for %s/%s", ri->uri,
              ri->query_string);
// CLIENT API
      if (!strcmp (ri->uri, "/new_session"))
        new_session (conn);
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
// Add option to update config.ini and to change scidb port that shim uses
// to talk to scidb? XXX finish this...
      else if (!strcmp (ri->uri, "/get_config"))
        mg_send_file (conn, cfgini);
//      else if (!strcmp (ri->uri, "/stop_scidb"))
//        stopscidb (conn, ri);
//      else if (!strcmp (ri->uri, "/start_scidb"))
//        startscidb (conn, ri);
      else if (!strcmp (ri->uri, "/get_log"))
        getlog(conn, ri);
      else
        {
// fallback to http file server
          if (!strcmp (ri->uri, "/"))
            snprintf (buf, MAX_VARLEN, "./wwwroot/index.html");
          else
            snprintf (buf, MAX_VARLEN, "./wwwroot%s", ri->uri);
          mg_send_file (conn, buf);
        }

// Mark as processed by returning non-null value.
      return "";
    }
  else
    {
      return NULL;
    }
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
            ("Usage:\nshim [-h] [-d] [-p <http port>] [-r <document root>] [-s <scidb port>]\n");
          printf ("Specify -f to run in the foreground.\nDefault http port is 8080.\nDefault SciDB port is 1239.\nDefault document root is /var/lib/shim/wwwroot.\n");
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
  struct mg_context *ctx;
  struct rlimit resLimit = {0};
  char *options[5];
  options[0] = "listening_ports";
  options[1] = DEFAULT_HTTP_PORT;
  options[2] = "document_root";
  options[3] = "/var/lib/shim/wwwroot";
  options[4] = NULL;
  parse_args (options, argc, argv, &daemonize);
  sessions = (session *) calloc (MAX_SESSIONS, sizeof (session));
  fdir = (char *) calloc (PATH_MAX,0);
  cfgini = (char *) calloc (PATH_MAX,0);
  snprintf (fdir, PATH_MAX, TMPDIR);

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
          getrlimit(RLIMIT_NOFILE, &resLimit);
          l = resLimit.rlim_max;
          for(j=0;j<l;j++) (void) close(j);
          j = open("/dev/null",O_RDWR); /* stdin */
          dup(j); /* stdout */
          dup(j); /* stderr */
          break;
        default:
          exit (0);
        }
    }

/* We locate the SciDB config.ini assuming that shim is installed in the SciDB
 * PATH. This is Linux-specific. Although SciDB is presently limited to Linux
 * anyway, this should really be made portable...
 */
  readlink("/proc/self/exe",cfgini,PATH_MAX);
  cfgini = dirname(cfgini);
  cfgini = strcat(cfgini, "/../etc/config.ini");

  openlog ("shim", LOG_CONS | LOG_NDELAY, LOG_USER);
  omp_init_lock (&lock);
  for (j = 0; j < MAX_SESSIONS; ++j)
    {
      sessions[j].sessionid = -1;
    }

  ctx = mg_start (&callback, NULL, (const char **) options);
  syslog (LOG_INFO,
          "SciDB HTTP service started on port(s) %s with web root [%s], talking to SciDB on port %d",
          mg_get_option (ctx, "listening_ports"),
          mg_get_option (ctx, "document_root"), SCIDB_PORT);

  for (;;)
    sleep (100);
  omp_destroy_lock (&lock);
  mg_stop (ctx);
  closelog ();
  free(cfgini);
  free(fdir);

  return 0;
}
