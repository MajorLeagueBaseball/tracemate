#ifndef TM_VERSION_H
#ifndef TM_BRANCH
#define TM_BRANCH "branches/master"
#endif
#ifndef TM_VERSION
#define TM_VERSION "b3ba65c10f98e96bda268b7b0c8b178c0e3b662e.1585149239"
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
