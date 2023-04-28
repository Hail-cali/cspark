#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define err(str) fprintf(stderr, str)

/*client code*/
/*----data structure------*/



typedef struct Json{
    /*
    Used for request query
    All the request and recv context is carried by json object
    session_id :: client fd
    id :: logical id for query
    book :: read buffer
     
     */
    int session_id;
    int id;
    int len;
    int req_ts;
    int processed_ts;
    char book[255];
    int cat;

    
} Json;


/*-----class----------*/

static volatile int SERVICE_KEEPALIVE; // Global status


typedef struct BinSemaphore{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int v;
} BinSemaphore;


typedef struct Job{
    struct Job* prev;
    void (*function)(void* args);
    void* args;
    Json* query;


} Job;

typedef struct JobQueue{
    pthread_mutex_t rmutex;
    Job* front;
    Job* rear;
    BinSemaphore* has_job;
    int len;
} JobQueue;




typedef struct Context {
    /*
    Used for processing query at the thread
    
    Each Context is independent but job-queue
    

    If job is allocated, 
    - Create stream
    - Request query to server
    - Recv result



     

     */
    int id;
    int fd;
    pthread_t pthread;
    int port;
    char inet[16];
    
    struct Driver* driver;
    struct sockaddr_in client_addr;

} Context;

typedef struct Driver {
    /*
    Virtualize Context from user
    
    Manage all the allocation, submit, and child thread
    process query with batch Context 
    
    
    - Create object
    - Recv query from user
    - Create json and submit to Context
    
    num_context:: Batch units that can be processed at one time
    
     
     
     */
    Context** contexts;
    int num_context;
    int run_forever ;
    char history[1024];
    int check;
    JobQueue job_queue;

} Driver;
/*--------------------------*/


/*-----call-----------------*/

static void* context_do(struct Context* thread_p);


static int _stream_init(Context*** thread_p);


static int bsem_init(BinSemaphore* bsem_p, int value);
static int job_queue_init(JobQueue* job_queue_p);

static volatile int SERVICE_KEEPALIVE;
/*-------------------------*/



static int job_queue_init(JobQueue* job_queue_p){
    
    job_queue_p -> front = NULL;
    job_queue_p -> rear = NULL;
    job_queue_p -> has_job = (struct BinSemaphore* )malloc(sizeof(struct BinSemaphore));
    job_queue_p -> len = 0;
    if (job_queue_p->has_job==NULL){
        err("job_queue_init():: binary setaphore allocation failed ");
        return -1;
    }
    pthread_mutex_init(&(job_queue_p->rmutex), NULL);
    bsem_init(job_queue_p->has_job, 0);
    return 0;
}

static int bsem_init(BinSemaphore* bsem_p, int value){
    

    pthread_mutex_init(&(bsem_p->mutex), NULL);
    pthread_cond_init(&(bsem_p->cond), NULL);
    bsem_p -> v = value;
        

    return 0;
}

static void bsem_post(BinSemaphore* bsem_p){
    pthread_mutex_lock(&bsem_p->mutex);
    bsem_p->v = 1;
    pthread_cond_signal(&bsem_p->cond);
    pthread_mutex_unlock(&bsem_p->mutex);

}

static void wait(BinSemaphore* bsem_p){

    pthread_mutex_lock(&bsem_p->mutex);
    while (bsem_p->v !=1){
        pthread_cond_wait(&bsem_p->cond, &bsem_p->mutex);
    }
    bsem_p-> v = 0;
    pthread_mutex_unlock(&bsem_p->mutex);


}

static void push(JobQueue* job_queue_p, struct Job* new_job){
    /*Broadcast to work thread*/
    pthread_mutex_lock(&job_queue_p->rmutex);
    new_job->prev = NULL;

    if (job_queue_p->len==0){
        job_queue_p->front = new_job;
        job_queue_p->rear = new_job;
    }    
    if (job_queue_p->len==1){
        new_job->prev = job_queue_p->front;
        job_queue_p->rear = job_queue_p->front;
        job_queue_p->front = new_job;
    }
    if (job_queue_p->len>=2){
        new_job->prev = job_queue_p->front;
        job_queue_p->front = new_job;
    }

    
    job_queue_p->len ++;
    bsem_post(job_queue_p->has_job);    
    pthread_mutex_unlock(&job_queue_p->rmutex);
}



static struct Job* pop(JobQueue* job_queue_p){
    pthread_mutex_lock(&job_queue_p->rmutex);

    Job* job_p = job_queue_p->front;
    
    if (job_queue_p->len==0){
    }
    else if (job_queue_p->len==1){
        job_queue_p->front = NULL;
        job_queue_p->rear = NULL;
        job_queue_p->len=0;

    }
    else if (job_queue_p->len>=2){
        job_queue_p->front = job_p->prev;
        job_queue_p->len --;
        bsem_post(job_queue_p->has_job);
    }
    
    pthread_mutex_unlock(&job_queue_p->rmutex);
    return job_p;
}


static int _stream_init(Context*** thread_p){
    Context* context_p = (**thread_p);
    context_p->port = 32209;
    char* tmp = "127.0.0.1";
    for (int i=0; i< strlen(tmp); i++){
        context_p->inet[i]= tmp[i];
    }
    int client_fd;
    int stream;
    if ((context_p->fd = socket(AF_INET, SOCK_STREAM, 0))< 0 ){
        printf("Socket creation error\n");
        return -1;

    }
    context_p->client_addr.sin_family = AF_INET;
    context_p->client_addr.sin_port = htons(context_p->port);
    if (inet_pton(AF_INET, context_p ->inet, &(context_p->client_addr.sin_addr))<= 0){
        err("_stream_init(): bind failed, invalid addrees\n");
    }
    stream = connect(context_p->fd, (struct sockaddr* )&(context_p->client_addr), sizeof(context_p->client_addr));
    if (stream < 0){
        err("_stream_init(): connection failed\n");
        return -1;
    }
    return 0;
}

static int _set_session(Context* context_p, Json* query){
    /*
    Set session info to qeury
    query doesn't know which Context take job, until Context get a job
    after that,  can put some infomation needed to processing stream
    
    *All the works done with functionly*


    */
    query-> session_id = context_p->fd;
    query -> id = context_p->id;
    query -> processed_ts = 12;
    return 0;
}

static void* context_do(struct Context* thread_p){
    /*
     Context process stream at the thread

     -Wait until new query added
     -put session_fd and meta infomation to Json* Qeury
     -call function Request(query)

     

     */
    /*fn for processing thread, same as excutor*/
    char debug_[17]= {0};
    snprintf(debug_, 17, "context-%d", thread_p->id);
    Driver* driver_p = thread_p->driver;

    while (SERVICE_KEEPALIVE){
        wait(driver_p->job_queue.has_job);
        if (SERVICE_KEEPALIVE && thread_p->fd >=0){
            //send(thread_p->client_fd, )
            void(* fn)(void* );
            void* args;
            Job* job_p = pop(&driver_p->job_queue);
            if(job_p){
                fn = job_p->function;
                _set_session(thread_p, job_p->query);    
                args = job_p->query;
                fn(args);

                free(job_p->query);
                free(job_p);
   //             close(thread_p->fd);
            }

        }

    }
   // close(thread_p->fd);
}


void request(void* args){
    /*
    Function for requst query to server
    -Read Json file
    -Send book name to server
    -Recv book contents from server
     
     

     */
    int valread;
    Json* query = (Json* )args;
    char txt_file[1024];
    printf("Thread #%d (%u) working on  book name (%s)\n", query->id,(int)pthread_self(), query->book );
    send(query->session_id, query->book, query->len, 0);
    printf("\t T[%d] send done \n", query->id);
    valread = read(query->session_id, txt_file, 1024);
    
    printf("\t T[%d] recv from server :: %s \n", query->id, txt_file);

}


static int add_query(Driver* driver, void (*function_p)(void* ),  void*args_p){
    /*
    Submit query to job-queue so that Context do work
    - create job contaiener
    - create Json shaped query
    - push_bach to job-queue
     

     */
    Job* new_job;
    Json* query;

    new_job = (struct Job*)malloc(sizeof(struct Job));
    if (new_job==NULL){
        err("at add_query():: could not allocated new job on memory");
        return -1;

    }

    query = (struct Json* )malloc(sizeof(struct Json));

    if (query ==NULL){
        err("at add_query():: could not allocated json on memory");
        return -1;
    }
    strcpy( query->book, (char* )args_p);

    query->req_ts = 10; 
    query->len = strlen(query->book);


    new_job->function =function_p;
    new_job->query = query;
    Driver* driver_p = driver;

    push(&driver_p->job_queue, new_job);

    return 0;
}



static int sample(Driver* driver){
    /*
    Read request book name from file
    submit to job-queue
     
    works functionaly

     */
    printf("TEST:: Sample query\n ");
    int i ; 
    FILE *fptr;
    int done;   
    char buf[50];


    fptr = fopen("./client/search_history.txt", "a+");

    while ( fscanf(fptr, "%s", buf)==1 )
        if (i < 10){
            done =  add_query(driver, request, (void* )(char* )buf);
        i+=1;
        }
        
    
    fclose(fptr);
    return 0;
}


static int context_init(Driver* driver, struct Context** thread_p, int id){
    /*
    generated context structure,
    - allocated itself at memory
    - connect pointer to main thread(driver)
    - make stream for TCP-IP which used for submit query to Cluster(server side)

    parmas: driver  : structure for main thread, gave ptr
          : tread_p :
    return: {0:done well, -1:fail to allocated}

     * */
    *thread_p = (struct Context* )malloc(sizeof(struct Context));
    if (*thread_p == NULL){
        err("context_init(): could not allocate moemory for thread");
        return -1;
    }
    (*thread_p)->driver = driver;
    (*thread_p)->id = id;

    if (_stream_init(&thread_p) <0){
        err("|_stream_init(): error\n");
        return -1;
    }
    
    printf("create context on thread\n");
    pthread_create(&(*thread_p)->pthread, NULL, (void * (*)(void *)) context_do, (*thread_p));
    pthread_detach((*thread_p)->pthread);
    return 0;

}


struct Driver* driver_init(int num_context){
    Driver* driver;

    SERVICE_KEEPALIVE =1;
    driver  = (struct Driver* )malloc(sizeof(struct Driver));
    if (driver == NULL){
        err("driver_init(): Could not allocate memory for driver\n");
        return NULL;
    }
    
    if (job_queue_init(&driver->job_queue)<0){
        err("driver_init(): could not allocate moemory for job queue");
            
        free(driver);
        return NULL;
    }

    /*make child thread(same as context) */
    driver->contexts = (struct Context** )malloc(num_context * sizeof(struct Context* ));
    if (driver->contexts==NULL){

        err("driver_init(): could not allocatie memory for context's thread");
        return NULL;
    }
    
    int n;
    for (n=0; n < num_context; n++ ){
        context_init(driver, &driver->contexts[n], n);

    }

    return driver;
}

int main(){
    Driver* driver  = driver_init(10);
    printf("--------------runing---------------- \n");
    /*test code on sample */
    sample(driver);
    while (SERVICE_KEEPALIVE){
        continue;
    }
    printf("-------------client end-------------\n");
    return 0;
}
