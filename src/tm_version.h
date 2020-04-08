#ifndef TM_VERSION_H
#ifndef TM_BRANCH
#define TM_BRANCH "branches/master"
#endif
#ifndef TM_VERSION
#define TM_VERSION "39d64bd443f36fb9dcd29a4e4e118c6bccc58847.1586198880"
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
