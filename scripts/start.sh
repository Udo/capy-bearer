#!/bin/bash

rm -r /tmp/bearer/work/* ; scripts/build_linux.sh && bin/bearer_fastcgi.linux.bin
