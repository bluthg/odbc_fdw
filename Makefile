##########################################################################
#
#                foreign-data wrapper for ODBC
#
# Copyright (c) 2011, PostgreSQL Global Development Group
#
# This software is released under the PostgreSQL Licence
#
# Author: Zheng Yang <zhengyang4k@gmail.com>
#
# IDENTIFICATION
#                 odbc_fdw/Makefile
# 
##########################################################################

MODULE_big = odbc_fdw
OBJS = odbc_fdw.o

EXTENSION = odbc_fdw
DATA = odbc_fdw--0.0.1.sql

REGRESS = odbc_fdw

SHLIB_LINK = -lodbc

ifdef DEBUG
override CFLAGS += -DDEBUG -g -O0
endif

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
