#!/bin/bash
set -e
mkdir -p /opt/my_blog/db
mkdir -p /opt/my_blog/file
cd /opt/my_blog/
rm -rf build blog_server
mkdir build
cd build
cmake ..
make
cp blog_server /opt/my_blog/
cd /opt/my_blog
ulimit -c unlimited
while [ 1 ]
do
    ./blog_server
done
