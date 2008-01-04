/**
 * Copyright (c) 2007- T Jake Luciani
 * Distributed under the New BSD Software License
 *
 * See accompanying file LICENSE or visit the Thrudb site at:
 * http://thrudb.googlecode.com
 *
 **/
#include "S3Backend.h"

#include "DistStore.h"
#include "utils.h"
#include "s3_glue.h"

#include <fstream>
#include <stdexcept>

using namespace s3;
using namespace std;
using namespace diststore;
using namespace log4cxx;
using namespace facebook::thrift::transport;
using namespace facebook::thrift::protocol;
using namespace facebook::thrift::concurrency;

#define memd MemcacheHandle::instance()

LoggerPtr S3Backend::logger(Logger::getLogger("S3Backend"));

S3Backend::S3Backend()
{
    LOG4CXX_INFO (logger, "S3Backend");
}

vector<string> S3Backend::getTablenames ()
{
    vector<string> tablenames;

    s3_result * result = list_buckets ();

    if (!result)
    {
        DistStoreException e;
        e.what = "HTTP error";
        throw e;
    }

    vector<Bucket *> contents = result->lambr->Buckets;
    vector<Bucket *>::iterator i;
    for (i = contents.begin (); i != contents.end (); i++)
    {
        tablenames.push_back ((*i)->Name);
    }

    return tablenames;
}

string S3Backend::get (const string & tablename, const string & key)
{
    class response_buffer *b = NULL;

    b = object_get (tablename, key, 0);

    if(b == NULL){
        DistStoreException e;
        e.what = "HTTP transport error";
        throw e;
    }

    if(b->result != 200) {
        DistStoreException e;
        e.what = "S3: " + key + " not found";
        throw e;
    }

    string result(b->base,b->len);
    delete b;

    return result;
}

void S3Backend::put (const string & tablename, const string & key,
                     const string & value)
{
    struct s3headers meta[2] = {{0,0},{0,0}};

    int r = object_put (tablename, key, value.c_str(), value.length(),
                        meta);

    if(r == -1){
        DistStoreException e;
        e.what = "HTTP error";
        throw e;
    }
}

void S3Backend::remove (const string & tablename, const string & key)
{
    int r = object_rm (tablename, key);

    if(r == -1){
        DistStoreException e;
        e.what = "HTTP error";
        throw e;
    }
}

ScanResponse S3Backend::scan (const string & tablename, const string & seed,
                              int32_t count)
{
    ScanResponse scan_response;

    s3_result * result = list_bucket (tablename, "", seed, count);

    if (!result)
    {
        DistStoreException e;
        e.what = "HTTP error";
        throw e;
    }

    vector<Contents *> contents = result->lbr->contents;
    vector<Contents *>::iterator i;
    for (i = contents.begin (); i != contents.end (); i++)
    {
        Element e;
        e.key = (*i)->Key;
        e.value = get (tablename, e.key);
        scan_response.elements.push_back (e);
    }

    scan_response.seed = scan_response.elements.size () > 0 ?
        scan_response.elements.back ().key : "";

    return scan_response;
}

string S3Backend::admin (const string & op, const string & data)
{
    return "";
}

void S3Backend::validate (const string * tablename, const string * key,
                          const string * value)
{
}
