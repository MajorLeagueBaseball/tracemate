#ifndef TM_VERSION_H
#ifndef TM_BRANCH
#define TM_BRANCH "branches/master"
#endif
#ifndef TM_VERSION
#define TM_VERSION "1df5e10e9dc28d58802c1772f4b8696d184dc57e.1599141552"
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
