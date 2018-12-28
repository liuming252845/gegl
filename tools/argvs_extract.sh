#!/bin/sh

echo "/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */"
echo "/* this file is autogenerated by $0 */"
echo "/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */"

# create function declarations for each item
cat $* | grep COMMAND_ARGS | grep ^int | sort | \
  sed -e 's/^int */int /' -e 's#(COMMAND_ARGS).*#(COMMAND_ARGS);#'

# create a function that registers all items found
echo "void argvs_register(void) {" 
cat $* | grep COMMAND_ARGS | grep ^int | sort | \
  sed -e 's/^int */ argvs_add (/ ' -e 's#(COMMAND_ARGS) */\*#, #' -e 's#\*/$#);#'
echo "};"
