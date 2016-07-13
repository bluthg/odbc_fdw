ODBC FDW for PostgreSQL 9.5+
============================

This PostgreSQL extension implements a Foreign Data Wrapper (FDW) for
remote databases using Open Database Connectivity [ODBC](http://msdn.microsoft.com/en-us/library/ms714562(v=VS.85).aspx).

This was originally developed by Zheng Yang <zhengyang4k@gmail.com> in 2011,
with contributions by Gunnar "Nick" Bluth <nick@pro-open.de> from 2014
and further developed by CARTO <dataservices@carto.com> since 2016.

Requirements
------------

To compile and install this extension, assuming a Linux OS,
the libraries and header files for ODBC and PostgreSQL are needed,
e.g. in Ubuntu this can be provided by the `unixodbc-dev`
and `postgresql-server-dev-9.5` system packages.

To make use of the extension ODBC drivers for the data sources to
be used must be installed in the system and reflected
in the `/etc/odbcinst.ini` file.

Building and Installing
-----------------------

The extension can be built and installed with:

```sh
make
sudo make install
```

Usage
-----

The `OPTION` clause of the `CREATE SERVER`, `CREATE FOREIGN TABLE`
and  `IMPORT FOREIGN SCHEMA` commands is used to define both
the ODBC attributes to define a connection to an ODBC data source
and some additional parameters to specify the table or query that
will be accessed as a foreign table.

The following options to define ODBC attributes should be defined in
the server definition (`CREATE SERVER`).

option   | description
-------- | -----------
`dsn`    | The Database Source Name of the foreign database system you're connecting to.
`driver` | The name of the ODBC driver to use (needed if no dsn is used)

These additional ODBC connection options are supported and can be defined
either in the server or foreign table definition (or in an IMPORT FOREIGN SCHEMA statement):

option     | description
---------- | -----------
`host`     | The address (hostname or ip) of the database server.
`port`     | The server port to connect to.
`database` | The name of the database to query.
`username` | The username to authenticate in the foreign server with.
`password` | The password to authenticate in the foreign server with.

The `username` and `password` options can also be defined
in a `CREATE USER MAPPING` statement, so that they are determined by
the connected PostgreSQL role.

The next options are used to define the table or query to connect a
foreign table to. They should be defined either in `CREATE FOREIGN TABLE`
or `IMPORT FOREIGN SCHEMA` statements:

option     | description
---------- | -----------
`schema`   | The schema of the database to query.
`table`    | The name of the table to query. Also the name of the foreign table to create in the case of queries.
`sql_query`| Optional: User defined SQL statement for querying the foreign table(s). This overrides the `table` parameters. This should use the syntax of ODBC driver used.
`sql_count`| Optional: User defined SQL statement for counting number of records in the foreign table(s). This should use the syntax of ODBC driver used.
`prefix`   | For IMPORT FOREIGN SCHEMA: a prefix for foreign table names. This can be used to prepend a prefix to the names of tables imported from an external database.

Example
-------

Assuming that the `odbc_fdw` is installed and available
in your database (`CREATE EXTENSION odbc_fdw`), and that
you have a DNS `test` defined for some ODBC datasource which
has a table named `dblist` in a schema named `test`:

```sql
CREATE SERVER odbc_server
  FOREIGN DATA WRAPPER odbc_fdw
  OPTIONS (dsn 'test');

CREATE FOREIGN TABLE
  odbc_table (
    id integer,
    name varchar(255),
    desc text,
    users float4,
    createdtime timestamp
  )
  SERVER odbc_server
  OPTIONS (
    database 'myplace',
    schema 'test',
    sql_query 'select description,id,name,created_datetime,sd,users from `test`.`dblist`',
    sql_count 'select count(id) from `test`.`dblist`'
  );

CREATE USER MAPPING FOR postgres
  SERVER odbc_server
  OPTIONS (username 'root', password '');
```

Note that no DSN is required; we can define connection attributes,
including the name of the ODBC driver, individually:

```sql
CREATE SERVER odbc_server
  FOREIGN DATA WRAPPER odbc_fdw
  OPTIONS (
    driver 'MySQL',
	host '192.168.1.17',
	encoding 'iso88591'
  );
```

The need to know about the columns of the table(s) to be queried
ad its types can be obviated by using the `IMPORT FOREIGN SCHEMA`
statement. By using the same OPTIONS as for `CREATE FOREIGN TABLE`
we can import as a foreign table the results of an arbitrary
query performed through the ODBC driver:

```sql
IMPORT FOREIGN SCHEMA test
  FROM SERVER odbc_server
  INTO public
  OPTIONS (
    database 'myplace',
    table 'odbc_table', -- this will be the name of the created foreign table
    sql_query 'select description,id,name,created_datetime,sd,users from `test`.`dblist`'
  );
```

LIMITATIONS
-----------

* Column, schema, table names should not be longer than the limit stablished by
  PostgreSQL ([NAMEDATALEN](https://www.postgresql.org/docs/9.5/static/sql-syntax-lexical.html#SQL-SYNTAX-IDENTIFIERS))
* Only the following column types are currently fully suported:
  - SQL_CHAR
  - SQL_WCHAR
  - SQL_VARCHAR
  - SQL_WVARCHAR
  - SQL_LONGVARCHAR
  - SQL_WLONGVARCHAR
  - SQL_DECIMAL
  - SQL_NUMERIC
  - SQL_INTEGER
  - SQL_REAL
  - SQL_FLOAT
  - SQL_DOUBLE
  - SQL_SMALLINT
  - SQL_TINYINT
  - SQL_BIGINT
  - SQL_DATE
  - SQL_TYPE_TIME
  - SQL_TIME
  - SQL_TIMESTAMP
  - SQL_GUID
* Option names must be lower-case.
* Only the ODBC connection attributes mentioned above can be provided via options:
  - `DSN`
  - `DRIVER`
  - `UID` (`username` option)
  - `PWD` (`password` option)
  - `SERVER` (`host` option)
  - `PORT`
  - `DATABASE`
* Foreign encodings are supported with the  `encoding` option
  for any enconding supported by PostgreSQL and compatible with the
  local database. The encoding must be identified with the
  name used by [PostgreSQL](https://www.postgresql.org/docs/9.5/static/multibyte.html).
