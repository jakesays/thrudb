#include <openssl/md5.h>
#include "MySQLBackend.h"
#include "ConfigFile.h"

/*
 * TODO:
 * - a caching system for the directory information, preferably one that lets
 *   us do lookups with it or else it's not that useful this should also help
 *   in the scan stuff, but is of much lower priority. can be local or shared
 * - cleanly recover from lost/broken connections
 * - look in to checkin's when exceptions are thrown
 * - look at libmemcached for it's partitioning algoritms
 * - think about straight key parititioning to allow in order scans etc...
 */

// protected
map<string, set<Partition*, bool(*)(Partition*, Partition*)>* > MySQLBackend::partitions;
string MySQLBackend::master_hostname;
int MySQLBackend::master_port;
string MySQLBackend::master_db;
string MySQLBackend::master_username;
string MySQLBackend::master_password;

// private
LoggerPtr MySQLBackend::logger (Logger::getLogger ("MySQLBackend"));

MySQLBackend::MySQLBackend ()
{
    LOG4CXX_INFO (logger, "MySQLBackend ()");
    master_hostname = ConfigManager->read<string> ("MYSQL_MASTER_HOSTNAME",
                                                   "localhost");
    LOG4CXX_INFO (logger, string ("master_hostname=") + master_hostname);
    master_port = ConfigManager->read<int> ("MYSQL_MASTER_PORT", 3306);
    {
        char buf[64];
        sprintf (buf, "master_port=%d\n", master_port);
        LOG4CXX_INFO (logger, buf);
    }
    master_db = ConfigManager->read<string> ("MYSQL_MASTER_DB", "mytable");
    LOG4CXX_INFO (logger, string ("master_db=") + master_db);
    master_username = ConfigManager->read<string> ("MYSQL_USERNAME", "mytable");
    LOG4CXX_INFO (logger, string ("master_username=") + master_username);
    master_password = ConfigManager->read<string> ("MYSQL_PASSWORD", "mytable");
}

void MySQLBackend::load_partitions (const string & tablename)
{
    set<Partition*, bool(*)(Partition*, Partition*)> *
        new_partitions = new set<Partition*, bool(*)(Partition*, Partition*)>
        (Partition::greater);

    Connection * connection = Connection::checkout
        (this->master_hostname.c_str (), this->master_db.c_str (),
         this->master_username.c_str (), this->master_password.c_str ());

    PreparedStatement * partitions_statement =
        connection->find_partitions_statement (tablename.c_str ());

    partitions_statement->execute ();

    PartitionsResults * pr =
        (PartitionsResults*)partitions_statement->get_bind_results ();

    while (partitions_statement->fetch () != MYSQL_NO_DATA)
    {
        LOG4CXX_INFO (logger, string ("load_partitions inserting: table=") +
                      pr->get_table () + string (", end=") + pr->get_end ());
        new_partitions->insert (new Partition (pr));
    }

    partitions[tablename] = new_partitions;
}

string MySQLBackend::get (const string & tablename, const string & key )
{
    FindReturn find_return = this->find_and_checkout (tablename, key);

    PreparedStatement * get_statement =
        find_return.connection->find_get_statement
        (find_return.data_tablename.c_str ());

    StringParams * tkp = (StringParams*)get_statement->get_bind_params ();
    tkp->set_key (key.c_str ());

    get_statement->execute ();

    string value;
    if (get_statement->fetch () == MYSQL_NO_DATA)
    {
        this->checkin (find_return.connection);
        MyTableException e;
        e.what = key + " not found in " + tablename;
        LOG4CXX_ERROR (logger, string ("get: ") + e.what);
        throw e;
    }

    KeyValueResults * kvr =
        (KeyValueResults*)get_statement->get_bind_results ();
    LOG4CXX_DEBUG (logger, string ("get: key=") + kvr->get_key () +
                   string (", value=") + kvr->get_value ());
    value = kvr->get_value ();

    this->checkin (find_return.connection);

    return value;
}

void MySQLBackend::put (const string & tablename, const string & key, const string & value)
{
    FindReturn find_return = this->find_and_checkout (tablename, key);

    PreparedStatement * put_statement =
        find_return.connection->find_put_statement
        (find_return.data_tablename.c_str ());

    StringStringParams * kvp = (StringStringParams*)put_statement->get_bind_params ();
    kvp->set_key (key.c_str ());
    kvp->set_value (value.c_str ());

    put_statement->execute ();

    this->checkin (find_return.connection);
}

void MySQLBackend::remove (const string & tablename, const string & key )
{
    FindReturn find_return = this->find_and_checkout (tablename, key);

    PreparedStatement * delete_statement =
        find_return.connection->find_delete_statement
        (find_return.data_tablename.c_str ());

    StringParams * kvp = (StringParams*)delete_statement->get_bind_params ();
    kvp->set_key (key.c_str ());

    delete_statement->execute ();

    this->checkin (find_return.connection);
}

string MySQLBackend::scan_helper (ScanResponse & scan_response,
                                  FindReturn & find_return,
                                  const string & seed,
                                  int32_t count)
{
    PreparedStatement * scan_statement =
        find_return.connection->find_scan_statement
        (find_return.data_tablename.c_str ());

    StringIntParams * kcp =
        (StringIntParams*)scan_statement->get_bind_params ();
    kcp->set_key (seed.c_str ());
    kcp->set_count (count);

    scan_statement->execute ();

    int ret;
    KeyValueResults * kvr =
        (KeyValueResults*)scan_statement->get_bind_results ();
    while ((ret = scan_statement->fetch ()) == 0)
    {
        // we gots results
        Element * e = new Element ();
        e->key = kvr->get_key ();
        e->value = kvr->get_value ();
        scan_response.elements.push_back (*e);
    }
    return string (kvr->get_key ());
}

/*
 * our seed will just be the last key returned. that's enough for us to find
 * the partition used last
 */
ScanResponse MySQLBackend::scan (const string & tablename, const string & seed,
                                 int32_t count)
{
    // base data_tablename should be > "0"
    FindReturn find_return;
    if (seed != "")
    {
        // subsequent call, use the normal find method
        find_return = this->find_and_checkout (tablename, seed);
    }
    else
    {
        // first call, get the first data_tablename
        find_return = this->find_next_and_checkout (tablename, "0");
    }
    LOG4CXX_DEBUG (logger, string ("scan: data_tablename=") +
                   find_return.data_tablename);

    ScanResponse scan_response;

    int size = 0;
    string offset = seed == "" ? string ("0") : seed;

more:
    // get data from our current find_return (parition) starting with values
    // greater than offset, returning count - size values
    offset = this->scan_helper (scan_response, find_return, offset,
                                count - size);
    // grab the current size of our returned elements
    size = (int)scan_response.elements.size ();

    // if we don't have enough elements
    if (scan_response.elements.size () < (unsigned int)count)
    {
        // try and find the next partition
        find_return = this->find_next_and_checkout (tablename,
                                                    find_return.data_tablename);
        if (find_return.connection != NULL)
        {
            // we have more partitions
            offset = "0"; // start at the begining of this new parition
            goto more; // goto's are fun :)
        }
    }

    // we're done now, return the last element as the seed so if there's more
    // data we'll know how to get at it
    scan_response.seed = scan_response.elements.size () > 0 ?
        scan_response.elements.back ().key : "";

    return scan_response;
}

FindReturn MySQLBackend::find_and_checkout (const string & tablename,
                                            const string & key)
{
    // we partition by the md5 of the key so that we'll get an even
    // distrobution of keys across partitions, we still store with key tho
    // TODO: is there a better way to do key -> md5 string
    unsigned char md5[16];
    memset (md5, 0, sizeof (md5));
    MD5 ((const unsigned char *)key.c_str (), key.length (), md5);
    string md5key;
    char hex[3];
    for (int i = 0; i < 16; i++)
    {
        sprintf (hex, "%02x", md5[i]);
        md5key += string (hex);
    }
    LOG4CXX_DEBUG (logger, string ("key=") + key + string (" -> md5key=") +
                   md5key);

    FindReturn find_return;

    // look for the partitions set
    set<Partition*, bool(*)(Partition*, Partition*)> * partitions_set = 
        partitions[tablename];

    if (partitions_set == NULL)
    {
        // we didn't find it, try loading
        this->load_partitions (tablename);
        partitions_set = partitions[tablename];
    }

    if (partitions_set != NULL)
    {
        // we now have the partitions set
        Partition * part = new Partition (md5key);
        // look for the matching partition
        set<Partition*>::iterator partition = partitions_set->lower_bound (part);
        // TODO how do we check if we got something "valid" back
        if (*partition)
        {
            LOG4CXX_DEBUG (logger, string ("found container, end=") +
                           (*partition)->get_end ());
            find_return.connection = Connection::checkout
                ((*partition)->get_host (), (*partition)->get_db (),
                 this->master_username.c_str (),
                 this->master_password.c_str ());
            find_return.data_tablename = (*partition)->get_table ();
            return find_return;
        }
        else
        {
            LOG4CXX_ERROR (logger, string ("table ") + tablename + 
                           string (" has a partitioning problem for key ") + 
                           key);
            MyTableException e;
            e.what = "MySQLBackend error";
            throw e;
        }
    }
    else
    {
        MyTableException e;
        e.what = tablename + " not found in directory";
        LOG4CXX_WARN (logger, string ("find_and_checkout: ") + e.what);
        throw e;
    }

    return find_return;
}

FindReturn MySQLBackend::find_next_and_checkout (const string & tablename,
                                                 const string & current_data_tablename)
{
    Connection * connection = Connection::checkout
        (this->master_hostname.c_str (), this->master_db.c_str (),
         this->master_username.c_str (), this->master_password.c_str ());

    PreparedStatement * next_statement =
        connection->find_next_statement (tablename.c_str ());

    StringParams * fpp = (StringParams*)next_statement->get_bind_params ();
    fpp->set_key (current_data_tablename.c_str ());

    next_statement->execute ();

    FindReturn find_return;
    find_return.connection = NULL;

    if (next_statement->fetch () == MYSQL_NO_DATA)
        return find_return;

    PartitionsResults * fpr =
        (PartitionsResults*)next_statement->get_bind_results ();

    find_return.data_tablename = fpr->get_table ();
    // if our connection to the master will work to get at the partition then
    // use it rather than checking out another, that will be
    if (connection->is_same (fpr->get_host (), fpr->get_db ()))
    {
        find_return.connection = connection;
    }
    else
    {
        find_return.connection = Connection::checkout
            (fpr->get_host (), fpr->get_db (),
             this->master_username.c_str (),
             this->master_password.c_str ());
        // if we're not returning our connection, check it back in
        Connection::checkin (connection);
    }

    LOG4CXX_DEBUG (logger, string ("data_tablename=") +
                   find_return.data_tablename);

    return find_return;
}

void MySQLBackend::checkin (Connection * connection)
{
    Connection::checkin (connection);
}

string MySQLBackend::admin (const string & op, const string & data)
{
    if (op == "load_partitions")
    {
        this->load_partitions (data);
        return "done";
    }
    return "";
}
