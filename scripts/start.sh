#!/bin/bash

rm -r /tmp/uce/work/* ; scripts/build_linux.sh && bin/uce_fastcgi.linux.bin
