#ifndef TM_VERSION_H
#ifndef TM_BRANCH
#define TM_BRANCH "branches/master"
#endif
#ifndef TM_VERSION
#define TM_VERSION "a3e4add30485a48ecb8c1ad4d6bcf99ec94836fc.1644868481"
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
