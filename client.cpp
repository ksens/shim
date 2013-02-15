/*
**
* BEGIN_COPYRIGHT
*
* Copyright (C) 2008-2012 SciDB, Inc.
*
* SciDB is free software: you can redistribute it and/or modify it under the
* terms of the GNU General Public License as published by the Free Software
* Foundation version 3 of the License.
*
* This software is distributed "AS-IS" AND WITHOUT ANY WARRANTY OF ANY KIND,
* INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY, NON-INFRINGEMENT, OR
* FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for the
* complete license terms.
*
* You should have received a copy of the GNU General Public License
* along with SciDB.  If not, see <http://www.gnu.org/licenses/>.
*
* END_COPYRIGHT
*/


/* Minimal SciDB Client Interface */
#include <string>
#include <iostream>
#include <stdio.h>
#include <fstream>
#include <algorithm>
#include <signal.h>

#define MAX_VARLEN 4096

#include "SciDBAPI.h"
using namespace std;

/* This structure is used to hand off a SciDB QueryResult object back into
 * C functions.
 */
struct prep
{
  unsigned long long queryid;
  void *queryresult;
};

/* Connect to a SciDB instance on the specified host and port.
 * Returns a pointer to the SciDB connection context, or NULL
 * if an error occurred.
 */
extern "C" void * scidbconnect(const char *host, int port)
{
  void* conn = NULL;
  const scidb::SciDB& db = scidb::getSciDB();
  try{
    conn = (void *)db.connect(host, port);
  } catch(std::exception& e)
  {
    conn=NULL;
  }
  return conn;
}

/* Disconnect a connected SciDB client connection,
 * the con pointer should be from the scidbconnect function above.
 * Does not return anything, but we should at least report errors XXX fix.
 */
extern "C" void scidbdisconnect(void * con)
{
  const scidb::SciDB& db = scidb::getSciDB();
  try{
    db.disconnect((void *)con);
  } catch(std::exception& e)
  {
  }
}

/* Execute a query, using the indicated SciDB client connection.
 * con SciDB connection context
 * query A query string to execute
 * afl = 0 use AQL otherwise afl
 * err place any character string errors that occur here
 * Returns a SciDB query ID on success, or zero on error (and writes the
 *  text of the error to the character buffer err).
 */
extern "C" unsigned long long
executeQuery(void *con, char *query, int afl, char *err)
{
  unsigned long long id = 0;
  const scidb::SciDB& db = scidb::getSciDB();
  const string &queryString = (const char *)query;
  scidb::QueryResult queryResult;
  try{
    db.executeQuery(queryString, bool(afl), queryResult, (void *)con);
    id = (unsigned long long)queryResult.queryID;
  } catch(std::exception& e)
  {
    snprintf(err,MAX_VARLEN,"%s",e.what());
  }
  return id;
}

/* Prepare a query using the indicated SciDB client connection.
 * result: Pointer to a struct prep, filled in by this function on success
 * con: a scidb connection context
 * afl = 0 use AQL, otherwise AFL.
 * err: a buff of length MAX_VARLEN to hold  error string should one occur
 *
 * Upon returning, either the results structure is populated with a non-NULL
 * queryresult pointer and a valid queryid and the err string is NULL, or
 * ther err string is not NULL and the queryresult is NULL.
 */
extern "C" void
prepare_query(void *result, void *con, char *query, int afl, char *err)
{
  struct prep *p;
  unsigned long long id = -1;
  const scidb::SciDB& db = scidb::getSciDB();
  const string &queryString = (const char *)query;
  scidb::QueryResult *q = new scidb::QueryResult();
  if(!q) 
  {
    snprintf(err,MAX_VARLEN,"Unable to allocate query result\n");
    return;
  }
  p = (struct prep *)result;
  try
  {
    db.prepareQuery(queryString, bool(afl), "", *q, con);
    p->queryresult = (void *)q;
    p->queryid = (unsigned long long)q->queryID;
  } catch(std::exception& e)
  {
    delete q;
    p->queryresult = NULL;
    snprintf(err,MAX_VARLEN,"%s",e.what());
  }
  return;
}

/* Execute a prepared requery stored in the prep structure pq on the scidb
 * connection context con. Set AFL to 0 for AQL query, to 1 for AFL query. The
 * char buffer err is a buffer of length MAX_VARLEN on input that will hold an
 * error string on output, should one occur. The queryresult object pointed to
 * from within pq is de-allocated by this function.  Successful exit returns
 * the queryid.  Failure populates the err buffer with an error string and
 * returns 0.
 */
extern "C" unsigned long long
execute_prepared_query(void *con, struct prep *pq, int afl, char *err)
{
  unsigned long long id = -1;
  const scidb::SciDB& db = scidb::getSciDB();
  scidb::QueryResult *q = (scidb::QueryResult *)pq->queryresult;
  if(!q)
  {
    snprintf(err,MAX_VARLEN,"Invalid query result object.\n");
    return 0;
  }
  try{
    db.executeQuery("", bool(afl), *q, (void *)con);
    id = pq->queryid;
  } catch(std::exception& e)
  {
    id = 0;
    snprintf(err,MAX_VARLEN,"%s",e.what());
  }
  delete q;
  pq->queryresult = NULL;
  return id;
}


/* Complete a SciDB query, where char buffer err is a buffer of length
 * MAX_VARLEN on input that will hold an error message should one occur.
 */
extern "C" void completeQuery(unsigned long long id, void *con, char *err)
{
  const scidb::SciDB& db = scidb::getSciDB();
  scidb::QueryID q = (scidb::QueryID)(id);
  try{
    db.completeQuery(q, (void *)con);
  } catch(std::exception& e)
  {
    snprintf(err,MAX_VARLEN,"%s",e.what());
  }
}
