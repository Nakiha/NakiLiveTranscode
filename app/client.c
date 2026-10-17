#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *base="http://127.0.0.1:8090", *action=NULL, *file=NULL;
    int i=1;
    if (i<argc && !strcmp(argv[i],"--url") && i+1<argc) {base=argv[i+1]; i+=2;}
    if (i<argc) action=argv[i++];
    if (action && (!strcmp(action,"start")||!strcmp(action,"watermark")) && i<argc) file=argv[i++];
    if (!action || i!=argc || (strcmp(action,"status") && strcmp(action,"health") && strcmp(action,"stop") && strcmp(action,"start") && strcmp(action,"watermark")) || ((!strcmp(action,"start")||!strcmp(action,"watermark"))&&!file)) {
        fprintf(stderr,"Usage: naki-live-ctl [--url http://127.0.0.1:8090] health|status|stop|start config.json|watermark update.json\n"); return 2;
    }
    char *body=NULL; size_t size=0;
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
    int n=snprintf(url,sizeof(url),"%s%s",base,!strcmp(action,"health")?"/health":!strcmp(action,"stop")?"/v1/job/stop":!strcmp(action,"watermark")?"/v1/job/watermark":"/v1/job");
    if (n<0 || n>=(int)sizeof(url)) {fprintf(stderr,"URL too long\n"); curl_easy_cleanup(curl); curl_global_cleanup(); free(body); return 1;}
    curl_easy_setopt(curl,CURLOPT_URL,url);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,3L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,15L);
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_PROTOCOLS_STR,"http,https");
    struct curl_slist *headers=NULL;
    if (file || !strcmp(action,"stop")) {
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
