// bootcore.h
// 2024, Aitor Gomez Garcia (info@aitorgomez.net)
// Thanks to Sorgelig and BBond007 for their help and advice in the development of this feature.

#ifndef __BOOTCORE_H__
#define __BOOTCORE_H__

char *getcoreName(char *path);
char *getcoreExactName(char *path);
char *replaceStr(const char *str, const char *oldstr, const char *newstr);
char *loadLastcore();
char *findCore(const char *name, char *coreName, int indent);
void bootcore_init(const char *path);

// resolve a core name (rbf basename, CASE-SENSITIVE - "MegaCD", not
// "MEGACD") to a full rbf path, preferring an exact match and
// otherwise the newest by date. 1 on success. wraps the same recursive
// scan bootcore uses; note the findCore declared above is stale and
// matches no definition in the tree, so do not try to use it.
int find_core_rbf(const char *coreName, char *out, int outsz);

extern char bootcoretype[64];
extern int16_t btimeout;

#endif // __BOOTCORE_H__
