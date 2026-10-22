#!/usr/bin/env bash

make USE_PGXS=1 clean
make USE_PGXS=1
make USE_PGXS=1 install

echo "==== Start install tests ===="
make USE_PGXS=1 installcheck