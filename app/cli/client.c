#include <curl/curl.h>
#include <yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Response {char data[65537];size_t size;} Response;
static int control_id(const char *id,size_t maximum) {
    return id&&*id&&strlen(id)<=maximum&&strspn(id,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==strlen(id);
}
static size_t receive(char *data,size_t unit,size_t count,void *opaque) {
    Response *r=opaque;
    if(unit&&count>65536/unit)return 0;
    size_t bytes=unit*count;
    if(bytes>65536-r->size)return 0;
    memcpy(r->data+r->size,data,bytes);r->size+=bytes;r->data[r->size]=0;return bytes;
}
/* Resolve once before the write. A later job replacement must reject this target. */
static int bind_target(CURL *curl,const char *base,char **body,size_t *size) {
    yyjson_doc *source=*body?yyjson_read(*body,*size,0):NULL;
    if(*body&&(!source||!yyjson_is_obj(yyjson_doc_get_root(source)))) {if(source)yyjson_doc_free(source);return -1;}
    if(source&&yyjson_obj_get(yyjson_doc_get_root(source),"job_id")) {yyjson_doc_free(source);return 0;}
    char url[4096];int length=snprintf(url,sizeof(url),"%s/v1/job",base);
    if(length<0||length>=(int)sizeof(url)) {if(source)yyjson_doc_free(source);return -1;}
    Response response={0};curl_easy_setopt(curl,CURLOPT_URL,url);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&response);
    CURLcode result=curl_easy_perform(curl);long code=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&code);
    yyjson_doc *status=result==CURLE_OK&&code==200?yyjson_read(response.data,response.size,0):NULL;
    const char *id=status?yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(status),"job_id")):NULL;
    yyjson_mut_doc *bound=id?(source?yyjson_doc_mut_copy(source,NULL):yyjson_mut_doc_new(NULL)):NULL;
    if(bound&&!source)yyjson_mut_doc_set_root(bound,yyjson_mut_obj(bound));
    char *encoded=NULL;size_t encoded_size=0;
    if(bound&&yyjson_mut_obj_add_strcpy(bound,yyjson_mut_doc_get_root(bound),"job_id",id))encoded=yyjson_mut_write(bound,0,&encoded_size);
    if(bound)yyjson_mut_doc_free(bound);
    if(status)yyjson_doc_free(status);
    if(source)yyjson_doc_free(source);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,NULL);curl_easy_setopt(curl,CURLOPT_WRITEDATA,stdout);
    if(!encoded)return -1;
    free(*body);*body=encoded;*size=encoded_size;return 0;
}

int main(int argc, char **argv) {
    const char *base="http://127.0.0.1:8090", *action=NULL, *file=NULL;
    const char *job=NULL,*operation=NULL;
    char *body=NULL; size_t size=0;
    int i=1;
    if (i<argc && !strcmp(argv[i],"--url") && i+1<argc) {base=argv[i+1]; i+=2;}
    if (i<argc) action=argv[i++];
    int targeted=action&&(!strcmp(action,"update")||!strcmp(action,"job")||!strcmp(action,"operation")||!strcmp(action,"stop-job"));
    if(targeted&&i<argc)job=argv[i++];
    if(action&&(!strcmp(action,"operation")||!strcmp(action,"stop-job"))&&i<argc)operation=argv[i++];
    int needs_file=action&&(!strcmp(action,"start")||!strcmp(action,"watermark")||!strcmp(action,"create")||!strcmp(action,"update"));
    if(needs_file&&i<argc)file=argv[i++];
    if (!action || i!=argc || (!targeted&&strcmp(action,"status")&&strcmp(action,"health")&&strcmp(action,"stop")&&strcmp(action,"start")&&strcmp(action,"watermark")&&strcmp(action,"create")) ||
        (needs_file&&!file)||(targeted&&!control_id(job,63))||((action&&(!strcmp(action,"operation")||!strcmp(action,"stop-job")))&&!control_id(operation,128))) {
        fprintf(stderr,"Usage: naki-live-ctl [--url URL] health|status|stop|start config.json|watermark update.json|create envelope.json|job JOB_ID|update JOB_ID patch.json|operation JOB_ID OPERATION_ID|stop-job JOB_ID OPERATION_ID\n"); return 2;
    }
    if(!strcmp(action,"stop-job")) {
        yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);yyjson_mut_val *root=d?yyjson_mut_obj(d):NULL;
        if(d) {yyjson_mut_doc_set_root(d,root);yyjson_mut_obj_add_strcpy(d,root,"operation_id",operation);body=yyjson_mut_write(d,0,&size);yyjson_mut_doc_free(d);}
        if(!body)return 1;
    }
    if (file) {
        FILE *f=fopen(file,"rb");
        if (!f) {perror(file); return 1;}
        body=malloc(16385);
        if (!body) {fclose(f); return 1;}
        size=fread(body,1,16385,f);
        int failed=ferror(f); fclose(f);
        if (failed || !size || size>16384) {fprintf(stderr,"Config must contain 1..16384 bytes\n"); free(body); return 1;}
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT)) {free(body); return 1;}
    CURL *curl=curl_easy_init();
    if (!curl) {free(body); curl_global_cleanup(); return 1;}
    char url[4096];
    int n;
    if(targeted) {
        if(!strcmp(action,"operation"))n=snprintf(url,sizeof(url),"%s/v1/jobs/%s/operations/%s",base,job,operation);
        else n=snprintf(url,sizeof(url),"%s/v1/jobs/%s%s",base,job,!strcmp(action,"update")?"/updates":!strcmp(action,"stop-job")?"/stop":"");
    } else n=snprintf(url,sizeof(url),"%s%s",base,!strcmp(action,"health")?"/health":!strcmp(action,"stop")?"/v1/job/stop":!strcmp(action,"watermark")?"/v1/job/watermark":!strcmp(action,"create")?"/v1/jobs":"/v1/job");
    if (n<0 || n>=(int)sizeof(url)) {fprintf(stderr,"URL too long\n"); curl_easy_cleanup(curl); curl_global_cleanup(); free(body); return 1;}
    curl_easy_setopt(curl,CURLOPT_URL,url);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,3L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,15L);
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_PROTOCOLS_STR,"http,https");
    if((!strcmp(action,"stop")||!strcmp(action,"watermark"))&&bind_target(curl,base,&body,&size)) {
        fprintf(stderr,"Cannot bind an explicit job target\n");curl_easy_cleanup(curl);curl_global_cleanup();free(body);return 1;
    }
    curl_easy_setopt(curl,CURLOPT_URL,url);
    struct curl_slist *headers=NULL;
    if (file || !strcmp(action,"stop")||!strcmp(action,"stop-job")) {
        headers=curl_slist_append(NULL,"Content-Type: application/json");
        curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
        curl_easy_setopt(curl,CURLOPT_POSTFIELDS,body?body:"");
        curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)size);
    }
    CURLcode ret=curl_easy_perform(curl);
    long code=0; curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&code);
    if (ret!=CURLE_OK) fprintf(stderr,"HTTP request failed: %s\n",curl_easy_strerror(ret));
    curl_slist_free_all(headers); curl_easy_cleanup(curl); curl_global_cleanup(); free(body);
    return ret==CURLE_OK && code>=200 && code<300 ? 0 : 1;
}
