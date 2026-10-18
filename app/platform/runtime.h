#ifndef NAKI_RUNTIME_H
#define NAKI_RUNTIME_H
#include <stdint.h>
#include <yyjson.h>
int64_t now_us(void);
void json_num(yyjson_mut_doc *doc, const char *name, double value);
void json_int(yyjson_mut_doc *doc, const char *name, int64_t value);
void json_str(yyjson_mut_doc *doc, const char *name, const char *value);
int mkdir_tree(const char *path);

#endif
