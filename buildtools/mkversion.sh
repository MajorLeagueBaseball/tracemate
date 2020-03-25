#!/bin/sh

STATUS=`git status 2>&1`
if [ $? -eq 0 ]; then
  echo "Building version info from git"
  HASH=`git show --format=%H | head -1`
  TSTAMP=`git show --format=%at | head -1`
  echo "    * version -> $HASH"
  SYM=`git symbolic-ref -q HEAD | awk -F'/' '{ print $NF }'`
  if [ -z "$SYM" ]; then
    SYM="detached"
  fi
  if [ -z "`echo $SYM | grep '^tags/'`" ]; then
    SYM="branches/$SYM"
  fi
  echo "    * symbolic -> $SYM"
  BRANCH=$SYM
  VERSION="$HASH.$TSTAMP"
  if [ -n "`echo $STATUS | grep 'Changed but not updated'`" ]; then
    VERSION="$HASH.modified.$TSTAMP"
  fi
else
  BRANCH=exported
  echo "    * exported"
fi

if [ -r "$1" ]; then
  eval `cat tm_version.h | awk '/^#define/ { print $2"="$3;}'`
  if [ "$NOIT_BRANCH" = "$BRANCH" -a "$NOIT_VERSION" = "$VERSION" ]; then
    echo "    * version unchanged"
    exit
  fi
fi

cat > $1 <<EOF
#ifndef TM_VERSION_H
#ifndef TM_BRANCH
#define TM_BRANCH "$BRANCH"
#endif
#ifndef TM_VERSION
#define TM_VERSION "$VERSION"
#endif

#include <stdio.h>
#include <string.h>

static inline int tm_build_version(char *buff, int len) {
  const char *start = TM_BRANCH;
  if(!strncmp(start, "branches/", 9))
    return snprintf(buff, len, "%s.%s", start+9, TM_VERSION);
  if(!strncmp(start, "tags/", 5))
    return snprintf(buff, len, "%s.%s", start+5, TM_VERSION);
  return snprintf(buff, len, "%s.%s", TM_BRANCH, TM_VERSION);
}

#endif
EOF
