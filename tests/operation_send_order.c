/* Test-only socket backpressure: hold a captured applied/pending RPC until
 * newer terminal history has actually been sent on the same worker channel. */
#include "ipc.h"
#include "pipeline_lifecycle_fixture.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int __real_ipc_send(int fd,yyjson_mut_doc *doc);
static int held,exported;
static int matches(yyjson_mut_val *record,const char *id) {
    const char *operation=yyjson_mut_get_str(yyjson_mut_obj_get(record,"operation_id"));return operation&&!strcmp(operation,id);
}
static void save(const char *directory,const char *name,yyjson_mut_doc *doc) {
    char path[4096];assert(snprintf(path,sizeof(path),"%s/%s",directory,name)<(int)sizeof(path));
    size_t bytes;char *text=yyjson_mut_write(doc,0,&bytes);assert(text);
    FILE *f=fopen(path,"w");assert(f);assert(fwrite(text,1,bytes,f)==bytes);assert(!fclose(f));free(text);
}
int __wrap_ipc_send(int fd,yyjson_mut_doc *doc) {
    naki_test_lifecycle_snapshot(doc);
    const char *directory=getenv("NAKI_TEST_OPERATION_ORDER_DIR"),*id=getenv("NAKI_TEST_OPERATION_ORDER_ID");
    if(!directory||!id)return __real_ipc_send(fd,doc);
    yyjson_mut_val *root=yyjson_mut_doc_get_root(doc);const char *kind=yyjson_mut_get_str(yyjson_mut_obj_get(root,"ipc_kind"));
    yyjson_mut_val *record=yyjson_mut_obj_get(root,"operation");
    const char *delivery=yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_obj_get(record,"delivery"),"state"));
    int pending=kind&&!strcmp(kind,"operation_reply")&&matches(record,id)&&delivery&&!strcmp(delivery,"pending");
    if(pending&&!exported) {if(!held)save(directory,"captured-pending.json",doc);held=1;errno=EAGAIN;return -1;}
    int final_page=0;
    if(held&&kind&&!strcmp(kind,"operations_page")) {
        yyjson_mut_val *records=yyjson_mut_obj_get(root,"operations");size_t i,max;yyjson_mut_val *value;
        yyjson_mut_arr_foreach(records,i,max,value) {
            const char *state=yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_obj_get(value,"delivery"),"state"));
            if(matches(value,id)&&state&&(!strcmp(state,"first_written")||!strcmp(state,"not_written")))final_page=1;
        }
    }
    int result=__real_ipc_send(fd,doc);
    if(!result&&final_page) {save(directory,"sent-final-page.json",doc);exported=1;}
    if(!result&&pending) {assert(exported);save(directory,"sent-old-pending.json",doc);}
    return result;
}
