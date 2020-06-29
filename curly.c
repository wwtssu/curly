#include "curly.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <curl/curl.h>

#ifndef _WIN32
#include <time.h>
#include <pthread.h>
#endif

void start_worker_thread_if_needed();
void stop_worker_thread();

static CURLM *multi_handle = NULL;
static int no_of_handles_running;
static int RUN_THREAD = 0;

struct curly_config my_config = {NULL, 0, 0, NULL};

//Forward declare platform specific thread functions
void create_worker_thread();
void signal_worker_thread();

#define LOG_MAX_BUF_SIZE 512
static void CURLY_LOG(const char* format, ...)
{
    if (my_config.log_cb) {
        static char buffer[LOG_MAX_BUF_SIZE];
        va_list args;
        va_start (args, format);
        vsnprintf (buffer,LOG_MAX_BUF_SIZE, format, args);
        va_end (args);
        my_config.log_cb(buffer);
    }
}

static int debug_func(CURL *handle, curl_infotype type,
             char *data, size_t size,
             void *userp)
{
    (void)handle; /* prevent compiler warning */
    
    if (type == CURLINFO_TEXT && (my_config.log_options & CURLY_LOG_INFO)) {
        CURLY_LOG("Info: %.*s", size, data);
    } else if (type == CURLINFO_HEADER_OUT && (my_config.log_options & CURLY_LOG_HEADERS)) {
        CURLY_LOG("Tx header: %.*s", size, data);
    } else if (type == CURLINFO_HEADER_IN && (my_config.log_options & CURLY_LOG_HEADERS)) {
        CURLY_LOG("Rx header: %.*s", size, data);
    } else if (type == CURLINFO_SSL_DATA_IN && (my_config.log_options & CURLY_LOG_DATA)) {
        CURLY_LOG("Rx SSL data: %.*s", size, data);
    } else if (type == CURLINFO_SSL_DATA_OUT && (my_config.log_options & CURLY_LOG_DATA)) {
        CURLY_LOG("Tx SSL data: %.*s", size, data);
    } else if (type == CURLINFO_DATA_IN && (my_config.log_options & CURLY_LOG_DATA)) {
        CURLY_LOG("Rx data: %.*s", size, data);
    } else if (type == CURLINFO_DATA_OUT && (my_config.log_options & CURLY_LOG_DATA)) {
        CURLY_LOG("Tx data: %.*s", size, data);
    }
    return 0;
}

typedef struct {
    char* data;
    long size;
    long size_left;
	void (*on_http_request_completed)(curly_http_transaction_handle handle, long http_response_code, void* data, long size);
    curly_http_transaction_handle* handle;
    struct curl_slist* headers;
} curly_http_transaction;

void init_curl_if_needed()
{
	if (multi_handle == NULL) {
		curl_global_init(CURL_GLOBAL_ALL);
		multi_handle = curl_multi_init();
        create_worker_thread();
	}
}

void curly_config_default(curly_config* cfg) {
    memset(cfg, 0, sizeof(curly_config));
}

void curly_init(curly_config* cfg) {
    //Release allocated memory in case the user calls curly_init without dispose
    if (my_config.certificate_path != NULL) {
        free(my_config.certificate_path);
        my_config.certificate_path = NULL;
    }
    memcpy(&my_config, cfg, sizeof(curly_config));
    if (cfg->certificate_path && (my_config.certificate_path = strdup(cfg->certificate_path)) == NULL) {
        CURLY_LOG("Error: Failed to duplicate certificate path. Certificate checks will not work.");
    }
    CURLY_LOG("Starting curly \nlog_options=%d and log_cb=0x%p \ndo_not_verify_peer=%d certificate_path=%s", my_config.log_options, my_config.log_cb, my_config.do_not_verify_peer, my_config.certificate_path != NULL ? my_config.certificate_path : "No certificate path provided");
    init_curl_if_needed();
}

void curly_dispose()
{
    stop_worker_thread();
    if (my_config.certificate_path != NULL) {
        free(my_config.certificate_path);
        my_config.certificate_path = NULL;
    }
    memset(&my_config, 0, sizeof(curly_config));
    curl_multi_cleanup(multi_handle);
    curl_global_cleanup();
	multi_handle = NULL;
}

curly_http_transaction* create_transaction(void* data, long size, void* cb)
{
	curly_http_transaction* transaction = (curly_http_transaction*)calloc(1, sizeof(curly_http_transaction));
	if (transaction == NULL) {
		return NULL;
	}

    if (size > 0 && data != NULL) {
        transaction->data = malloc(size);
        if (transaction->data == NULL) {
            CURLY_LOG("Error: Failed to allocate memory for transaction");
            free(transaction);
            return NULL;
        } else {
           memcpy(transaction->data, data, size);
        }
    }

    transaction->size = size;
    transaction->size_left = size;
    transaction->handle = curl_easy_init();
    if (!transaction->handle) {
        free(transaction->data);
        free(transaction);
        return NULL;
    }
	transaction->on_http_request_completed = cb;
    
    transaction->headers = NULL;
    return transaction;
}

static void cleanup_transaction(curly_http_transaction* transaction) {
	curl_multi_remove_handle(multi_handle, transaction->handle);
	curl_easy_cleanup(transaction->handle);
	if (transaction->data) {
		free(transaction->data);
	}
    if(transaction->headers) {
        curl_slist_free_all(transaction->headers);
    }
	free(transaction);
}

static int poll() {
	int numfds = 0;
	CURLMsg *cmsg = NULL;
	CURLcode easy_status = CURLE_OK;
	int msgs_in_queue = 0;
	int res = curl_multi_wait(multi_handle, NULL, 0, 100, &numfds);
	if (res != CURLM_OK) {
		CURLY_LOG("error: curl_multi_wait() returned %d\n", res);
        return EXIT_FAILURE;
	}

	curl_multi_perform(multi_handle, &no_of_handles_running);

	while ((cmsg = curl_multi_info_read(multi_handle, &msgs_in_queue))) {
		if (cmsg->msg == CURLMSG_DONE) {
			CURL *easy_handle = cmsg->easy_handle;
			curly_http_transaction* transaction = NULL;
			long http_response_code = 0;
			if (cmsg->data.result != CURLE_OK) {
				CURLY_LOG("Error: result != CURLE_OK %d", cmsg->data.result);
				continue;
			}

			easy_status = curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, (void*)&transaction);
			if (easy_status != CURLE_OK || transaction == NULL) {
				CURLY_LOG("Error retreiving private pointer");
				continue;
			}
            
			curl_easy_getinfo(easy_handle, CURLINFO_RESPONSE_CODE, &http_response_code);
            
			if (transaction->on_http_request_completed) {
				transaction->on_http_request_completed(transaction->handle, http_response_code, transaction->data, transaction->size);
			}
			
            //TODO, how handle retries of failed transactions
			cleanup_transaction(transaction);
		}
	}
	return no_of_handles_running;
}

//Since the json array with the headers is really simple we do the parsing manually instead of adding a dependency
//to an external parser like jsmn.
static void add_custom_headers(CURL *http_get_handle, curly_http_transaction* transaction, const char* headers_json) {
    CURLcode easy_status = CURLE_OK;
    if(headers_json != NULL && strlen(headers_json) > 2) {
        char* walker_p = (char*)headers_json;
        char header_buf[1024];
        CURLY_LOG("Using customer headers %s", headers_json);
        if (*walker_p != '{' || *(walker_p + strlen(headers_json) -1) != '}')  {
            CURLY_LOG("Incorrect json format. The json string must start with { and end with }");
            return;
        }
       
        while (walker_p != NULL) {
            int i = 0;
            char* end_p = strstr(walker_p+1, ",\"");
            if (end_p == NULL) {
                end_p = strstr(walker_p, "}");
                if (end_p == NULL) {
                    CURLY_LOG("Error: no closing } found in json string");
                    return;
                }
            }
            walker_p++;
            //We must strip out the "" from the json elements since the server does not like headers with ""
            do {
                if (*walker_p != '\"') {
                    header_buf[i++] = *walker_p;
                }
                
            } while(++walker_p != end_p);
            header_buf[i] = '\0';
            CURLY_LOG("Passing header %s to curl", header_buf);
            transaction->headers = curl_slist_append(transaction->headers, header_buf);
            walker_p = strstr(end_p, ",\"");
        }
        easy_status = curl_easy_setopt(http_get_handle, CURLOPT_HTTPHEADER, transaction->headers);
        if (easy_status != CURLE_OK) {
            CURLY_LOG("curl_easy_setopt with param CURLOPT_HTTPHEADER failed with error %d for header=%s", easy_status, header_buf);
        }
    }
}


static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
	size_t realsize = size * nmemb;
	curly_http_transaction *transaction = (curly_http_transaction*)userdata;
	void* realloc_mem = realloc(transaction->data, transaction->size + realsize + 1);
	
	if (realloc_mem == NULL) {
		/* out of memory! */
		CURLY_LOG("not enough memory (realloc returned NULL)\n");
		return 0;
	}
	else {
		transaction->data = realloc_mem;
	}

	memcpy(&(transaction->data[transaction->size]), ptr, realsize);
	transaction->size += realsize;
	transaction->data[transaction->size] = '\0';
	//printf("%s", transaction->data);
	return realsize;
}

curly_http_transaction_handle curly_http_get(const char* url, const char* headers_json, void* cb)
{
	CURLcode easy_status = CURLE_OK;
    CURLMcode status = CURLM_OK;
    CURL *http_get_handle;
    curly_http_transaction* transaction = create_transaction(NULL, 0, cb);
    if (transaction == NULL) {
        CURLY_LOG("Error: Failed to create curly transaction. The GET operation can not be performed");
        return NULL;
    }
    http_get_handle = transaction->handle;

    CURLY_LOG("Starting http GET to %s", url);
    
    /* set options */
    curl_easy_setopt(http_get_handle, CURLOPT_URL, url);

    if (my_config.do_not_verify_peer) {
        curl_easy_setopt(http_get_handle, CURLOPT_SSL_VERIFYPEER, 0L);
        CURLY_LOG("Warning: VERIFYPEER turned off");
    } else {
        if (my_config.certificate_path != NULL) {
            easy_status = curl_easy_setopt(http_get_handle, CURLOPT_CAINFO, my_config.certificate_path);
            if (easy_status != CURLE_OK) {
                CURLY_LOG("Error: Failed to add certificate bundle: %s. \nPeer Verification will not be enabled.", my_config.certificate_path);
                curl_easy_setopt(http_get_handle, CURLOPT_SSL_VERIFYPEER, 0L);
            }
        }
    }

    if(my_config.log_options != 0) {
        curl_easy_setopt(http_get_handle, CURLOPT_DEBUGFUNCTION, &debug_func);
        curl_easy_setopt(http_get_handle, CURLOPT_VERBOSE, 1L);
    }

    if (my_config.no_signal) {
        easy_status = curl_easy_setopt(http_get_handle, CURLOPT_NOSIGNAL, 1L);
        if (easy_status != CURLE_OK) {
            CURLY_LOG("Error: Failed to set CURLOPT_NOSIGNAL with error %d", easy_status);
        }
    }
    
	/* send all data to this function  */
	curl_easy_setopt(http_get_handle, CURLOPT_WRITEFUNCTION, &write_callback);

	/* we pass our 'chunk' struct to the callback function */
	curl_easy_setopt(http_get_handle, CURLOPT_WRITEDATA, (void *)transaction);

	easy_status = curl_easy_setopt(http_get_handle, CURLOPT_PRIVATE, (void*)transaction); 
	if (easy_status != CURLE_OK) {
		CURLY_LOG("Failed setting private data.");
	}
    
    add_custom_headers(http_get_handle, transaction, headers_json);
    
    /* add the individual transfers */
    status = curl_multi_add_handle(multi_handle, http_get_handle);
    if (status != CURLM_OK) {
        CURLY_LOG("curl_multi_add_handle failed with error %d", status);
        return NULL;
    }

	signal_worker_thread();
    return transaction;
}

//Read callback for fetching data to put or post
static size_t read_callback(void *ptr, size_t size, size_t nmemb, void *userp)
{
    curly_http_transaction *transaction = (curly_http_transaction*)userp;
    long bytes_read = 0;
    CURLY_LOG("We are asked to provide at most %lu bytes", size*nmemb);
    if (size*nmemb < 1)
        return bytes_read;

    if (size*nmemb > transaction->size_left) {
        //This chunk can handle the whole transaction
		bytes_read = transaction->size_left;
		memcpy(ptr, transaction->data + (transaction->size - transaction->size_left), bytes_read);
        transaction->size_left = 0;
    } else {
        //This chunk can only handle part of the transaction
		bytes_read = (int)(size*nmemb);
		memcpy(ptr, transaction->data + (transaction->size - transaction->size_left), bytes_read);
        transaction->size_left = (int)(transaction->size_left - bytes_read);
    }
    return bytes_read;
}

curly_http_transaction_handle curly_http_put(const char* url, void* data, long size, const char* headers_json, void* cb)
{
	CURLcode easy_status = CURLE_OK;
    CURLMcode status = CURLM_OK;
    CURL *http_put_handle;
    curly_http_transaction* transaction = create_transaction(data, size, cb);
    if (transaction == NULL) {
        CURLY_LOG("Error: Failed to create curly transaction. The PUT operation can not be performed");
        return NULL;
    }
    
    http_put_handle = transaction->handle;

    CURLY_LOG("Starting http PUT to %s", url);
    
    /* set options */
    curl_easy_setopt(http_put_handle, CURLOPT_URL, url);
    
    if(my_config.log_options != 0) {
        curl_easy_setopt(http_put_handle, CURLOPT_DEBUGFUNCTION, &debug_func);
        curl_easy_setopt(http_put_handle, CURLOPT_VERBOSE, 1L);
    }
    
    if (my_config.do_not_verify_peer) {
        curl_easy_setopt(http_put_handle, CURLOPT_SSL_VERIFYPEER, 0L);
        CURLY_LOG("Warning: VERIFYPEER turned off");
    } else {
        if (my_config.certificate_path != NULL) {
            easy_status = curl_easy_setopt(http_put_handle, CURLOPT_CAINFO, my_config.certificate_path);
            if (easy_status != CURLE_OK) {
                CURLY_LOG("Error: Failed to add certificate bundle: %s. \nPeer Verification will not be enabled.", my_config.certificate_path);
                curl_easy_setopt(http_put_handle, CURLOPT_SSL_VERIFYPEER, 0L);
            }
        }
    }
    
    if (my_config.no_signal) {
        easy_status = curl_easy_setopt(http_put_handle, CURLOPT_NOSIGNAL, 1L);
        if (easy_status != CURLE_OK) {
            CURLY_LOG("Error: Failed to set CURLOPT_NOSIGNAL with error %d", easy_status);
        }
    }
    
    /* enable uploading */
    curl_easy_setopt(http_put_handle, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(http_put_handle, CURLOPT_INFILESIZE, size);
    
    /* we want to use our own read function */
    curl_easy_setopt(http_put_handle, CURLOPT_READFUNCTION, read_callback);

    /* pointer to pass to our read function */
    curl_easy_setopt(http_put_handle, CURLOPT_READDATA, transaction);

	/* Store our transaction pointer */
	easy_status = curl_easy_setopt(http_put_handle, CURLOPT_PRIVATE, (void*)transaction);
	if (easy_status != CURLE_OK) {
		CURLY_LOG("Failed setting private data.");
	}
    
    add_custom_headers(http_put_handle, transaction, headers_json);

    /* Add the easy handle to the multi handle */
    status = curl_multi_add_handle(multi_handle, http_put_handle);
    if (status != CURLM_OK) {
		CURLY_LOG("curl_multi_add_handle failed with error %d", status);
        return NULL;
    }

	signal_worker_thread();
    return transaction;
}

curly_http_transaction_handle curly_http_post(const char* url, void* data, long size, const char* headers_json, void* cb)
{
    CURLcode easy_status = CURLE_OK;
    CURLMcode status = CURLM_OK;
    CURL *http_post_handle;
    curly_http_transaction* transaction = create_transaction(data, size, cb);
    if (transaction == NULL) {
        CURLY_LOG("Error: Failed to create curly transaction. The POST operation can not be performed");
        return NULL;
    }
    
    http_post_handle = transaction->handle;
    
    CURLY_LOG("Starting http POST to %s", url);
    
    /* set options */
    curl_easy_setopt(http_post_handle, CURLOPT_URL, url);
    
    if(my_config.log_options != 0) {
        curl_easy_setopt(http_post_handle, CURLOPT_DEBUGFUNCTION, &debug_func);
        curl_easy_setopt(http_post_handle, CURLOPT_VERBOSE, 1L);
    }
    
    if (my_config.do_not_verify_peer) {
        curl_easy_setopt(http_post_handle, CURLOPT_SSL_VERIFYPEER, 0L);
        CURLY_LOG("Warning: VERIFYPEER turned off");
    } else {
        if (my_config.certificate_path != NULL) {
            easy_status = curl_easy_setopt(http_post_handle, CURLOPT_CAINFO, my_config.certificate_path);
            if (easy_status != CURLE_OK) {
                CURLY_LOG("Error: Failed to add certificate bundle: %s. \nPeer Verification will not be enabled.", my_config.certificate_path);
                curl_easy_setopt(http_post_handle, CURLOPT_SSL_VERIFYPEER, 0L);
            }
        }
    }
    
    if (my_config.no_signal) {
        easy_status = curl_easy_setopt(http_post_handle, CURLOPT_NOSIGNAL, 1L);
        if (easy_status != CURLE_OK) {
            CURLY_LOG("Error: Failed to set CURLOPT_NOSIGNAL with error %d", easy_status);
        }
    }
    
    /* enable uploading */
    curl_easy_setopt(http_post_handle, CURLOPT_POST, 1L);
    curl_easy_setopt(http_post_handle, CURLOPT_POSTFIELDSIZE, size);
    curl_easy_setopt(http_post_handle, CURLOPT_INFILESIZE, size);
    
    /* we want to use our own read function */
    curl_easy_setopt(http_post_handle, CURLOPT_READFUNCTION, read_callback);
    
    /* pointer to pass to our read function */
    curl_easy_setopt(http_post_handle, CURLOPT_READDATA, transaction);
    
    /* Store our transaction pointer */
    easy_status = curl_easy_setopt(http_post_handle, CURLOPT_PRIVATE, (void*)transaction);
    if (easy_status != CURLE_OK) {
        CURLY_LOG("Failed setting private data.");
    }
    
    add_custom_headers(http_post_handle, transaction, headers_json);
    
    /* Add the easy handle to the multi handle */
    status = curl_multi_add_handle(multi_handle, http_post_handle);
    if (status != CURLM_OK) {
        CURLY_LOG("curl_multi_add_handle failed with error %d", status);
        return NULL;
    }
    
    signal_worker_thread();
    return transaction;
}

curly_http_transaction_handle curly_http_delete(const char* url, void* data, long size, const char* headers_json, void* cb)
{
    CURLcode easy_status = CURLE_OK;
    CURLMcode status = CURLM_OK;
    CURL *http_delete_handle;
    curly_http_transaction* transaction = create_transaction(data, size, cb);
    if (transaction == NULL) {
        CURLY_LOG("Error: Failed to create curly transaction. The DELETE operation can not be performed");
        return NULL;
    }
    
    http_delete_handle = transaction->handle;
    
    CURLY_LOG("Starting http DELETE to %s", url);
    
    /* set options */
    curl_easy_setopt(http_delete_handle, CURLOPT_URL, url);
    
    if(my_config.log_options != 0) {
        curl_easy_setopt(http_delete_handle, CURLOPT_DEBUGFUNCTION, &debug_func);
        curl_easy_setopt(http_delete_handle, CURLOPT_VERBOSE, 1L);
    }
    
    if (my_config.do_not_verify_peer) {
        curl_easy_setopt(http_delete_handle, CURLOPT_SSL_VERIFYPEER, 0L);
        CURLY_LOG("Warning: VERIFYPEER turned off");
    } else {
        if (my_config.certificate_path != NULL) {
            easy_status = curl_easy_setopt(http_delete_handle, CURLOPT_CAINFO, my_config.certificate_path);
            if (easy_status != CURLE_OK) {
                CURLY_LOG("Error: Failed to add certificate bundle: %s. \nPeer Verification will not be enabled.", my_config.certificate_path);
                curl_easy_setopt(http_delete_handle, CURLOPT_SSL_VERIFYPEER, 0L);
            }
        }
    }
    
    if (my_config.no_signal) {
        easy_status = curl_easy_setopt(http_delete_handle, CURLOPT_NOSIGNAL, 1L);
        if (easy_status != CURLE_OK) {
            CURLY_LOG("Error: Failed to set CURLOPT_NOSIGNAL with error %d", easy_status);
        }
    }
    
    /* enable uploading */
    curl_easy_setopt(http_delete_handle, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(http_delete_handle, CURLOPT_INFILESIZE, size);
    
    /* we want to use our own read function */
    curl_easy_setopt(http_delete_handle, CURLOPT_READFUNCTION, read_callback);
    
    /* pointer to pass to our read function */
    curl_easy_setopt(http_delete_handle, CURLOPT_READDATA, transaction);
    
    /* Store our transaction pointer */
    easy_status = curl_easy_setopt(http_delete_handle, CURLOPT_PRIVATE, (void*)transaction);
    if (easy_status != CURLE_OK) {
        CURLY_LOG("Failed setting private data.");
    }
    
    add_custom_headers(http_delete_handle, transaction, headers_json);
    
    /* Add the easy handle to the multi handle */
    status = curl_multi_add_handle(multi_handle, http_delete_handle);
    if (status != CURLM_OK) {
        CURLY_LOG("curl_multi_add_handle failed with error %d", status);
        return NULL;
    }
    
    signal_worker_thread();
    return transaction;
}

/*
 * Internal worker thread handling. 
 * Only active if there are transfers in progress.
 */
#ifdef _WIN32
HANDLE thread_handle = NULL;
DWORD WINAPI worker_thread(LPVOID lpParam)
{
	do {
		poll();
		Sleep(20);
	} while (no_of_handles_running > 0);
	CURLY_LOG("stopping worker thread");
	CloseHandle(thread_handle);
	thread_handle = NULL;
	return 0;
}

void create_worker_thread() {
    if (thread_handle == NULL) {
        CURLY_LOG("starting worker thread");
        thread_handle = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
    }
}

void signal_worker_thread() {
    CURLY_LOG("TODO, IMPLEMENT WaitForSingleObject for Win32");
}

void stop_worker_thread() {
    CURLY_LOG("Stopping worker thread. TODO win32 waitforsingleobject");
}
#else 
pthread_t thread;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cv  = PTHREAD_COND_INITIALIZER;;

void *worker_thread(void *threadid)
{
    RUN_THREAD = 1;
    do {
        poll();
        if (no_of_handles_running == 0) {
            CURLY_LOG("Wait for next job.");
            pthread_mutex_lock(&mutex);
            pthread_cond_wait(&cv, &mutex);
            if (RUN_THREAD) {
                CURLY_LOG("Job received");
            } else {
                CURLY_LOG("Shutdown signal received");
            }
            pthread_mutex_unlock(&mutex);
        }
    } while (RUN_THREAD);
    CURLY_LOG("Worker thread about to exit.");
    pthread_exit(NULL);
    
}
void create_worker_thread() {
   
    int rc = pthread_create(&thread, NULL, worker_thread, NULL);
    if (rc){
        CURLY_LOG("ERROR; return code from pthread_create() is %d", rc);
        return;
    } else {
        CURLY_LOG("Starting posix worker thread");
    }
}

void signal_worker_thread() {
    pthread_mutex_lock(&mutex);
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mutex);
}

void stop_worker_thread() {
    CURLY_LOG("Stopping posix worker thread");
    RUN_THREAD = 0;
    pthread_mutex_lock(&mutex);
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mutex);
    CURLY_LOG("Waiting for thread to join");
    pthread_join(thread, NULL);
    CURLY_LOG("Thread has joined. Curly has shutdown.");
}
#endif
