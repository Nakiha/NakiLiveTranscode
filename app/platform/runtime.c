#include "runtime.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <libavutil/time.h>
int64_t now_us(void) { return av_gettime_relative(); }
void json_num(yyjson_mut_doc *doc, const char *name, double value) {
    if (doc) {yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),name);yyjson_mut_obj_add_real(doc, yyjson_mut_doc_get_root(doc), name, value);}
}
void json_int(yyjson_mut_doc *doc, const char *name, int64_t value) {
    if (doc) {yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),name);yyjson_mut_obj_add_sint(doc, yyjson_mut_doc_get_root(doc), name, value);}
}
void json_str(yyjson_mut_doc *doc, const char *name, const char *value) {
    if (doc) {yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),name);yyjson_mut_obj_add_strcpy(doc, yyjson_mut_doc_get_root(doc), name, value);}
}
int mkdir_tree(const char *path) {
    char *copy = strdup(path);
    if (!copy) return -1;
    for (char *p=copy+1; *p; p++) if (*p=='/') {
        *p=0;
        if (mkdir(copy,0755)<0 && errno!=EEXIST) {free(copy); return -1;}
        *p='/';
    }
    int ret=mkdir(copy,0755);
    if (ret<0 && errno==EEXIST) {struct stat st; ret=stat(copy,&st)==0 && S_ISDIR(st.st_mode) ? 0 : -1;}
    free(copy); return ret;
}
