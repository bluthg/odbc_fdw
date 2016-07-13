/*-------------------------------------------------------------------------
 *
 *                foreign-data wrapper for ODBC
 *
 * Copyright (c) 2011, PostgreSQL Global Development Group
 * Copyright (c) 2016, CARTO
 *
 * This software is released under the PostgreSQL Licence
 *
 * Author: Zheng Yang <zhengyang4k@gmail.com>
 *
 * IDENTIFICATION
 *                odbc_fdw/odbc_fdw--0.0.1.sql
 *
 *-------------------------------------------------------------------------
 */

CREATE FUNCTION odbc_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION odbc_fdw_validator(text[], oid)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER odbc_fdw
  HANDLER odbc_fdw_handler
  VALIDATOR odbc_fdw_validator;
