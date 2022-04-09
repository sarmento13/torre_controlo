//abdellahi ould cheikh brahim
//maria joão teixeira sarmento
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/msg.h>
#include <time.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/time.h>
#include <semaphore.h>
#include <sys/stat.h>

//#define DEBUG
#define PIPE_NAME "input_pipe"
#define BUF_SIZE 1024
#define BILLION  1000000000.0;
#define ARRIVE 2;
#define PRIORITY_ARRIVE 1;
#define DEPART 3;

//all structs
typedef struct{
    int type;
    char flight_code[255];
    int sendTime;
    int fuel;
    int slot;
}tower_struct;
typedef struct{
    long mytype;
    char flight_code[255]; //flight code
    int init; //initial mainTime
    int takeoff; //takeoff mainTime
    //only arrivals
    int eta; //mainTime to runaway
    int fuel; //fuel
}flight_struct;
typedef struct{
    long mytype;
    int slot;
}slot_struct;
typedef struct{
    pthread_mutex_t mutex;
    pthread_cond_t condVar;
}cond_struct;
typedef struct{
    int fuel;
    int holding;
    int autorization;
    cond_struct cond;
}tower_queue;
typedef struct{
    int num_created;    //Número total de voos criados
    int num_depart;     //Número total de voos que descolaram
    int num_arrive;     //Número total de voos que aterraram
    int wait_arrive;    //Tempo médio de espera (para além do ETA) para aterrar
    int wait_depart;    //Tempo médio de espera para descolar
    int holding_arrive; //Número médio de manobras de holding por voo de aterragem
    int urgence;//Número vôs de urgência
    int redirect;       //Número de voos redirecionados para outro aeroporto
    int rejected;       //Voos rejeitados pela Torre de Controlo
    tower_queue *flight_slot;
}shared_struct;
typedef struct{
    int time_unit;      //unidade de tempo (ut, em milissegundos)
    int depart;         //duração da descolagem (T, em ut)
    int delta_depart;   //intervalo entre descolagem (dt, em ut)
    int arrive;         //duração da aterragem (L, em ut)
    int delta_arrive;   //intervalo entre aterragens (dl, em ut)
    int hold_min;       //holding duração mínima (min, em ut)
    int hold_max;       //holding duração máxima (max, em ut)
    int max_depart;     //quantidade máxima de partidas no sistema (D)
    int max_arrive;     //quantidade máxima de chegadas no sistema (A)
}config_struct;
//all list
typedef struct flight_node *flight_list;
typedef struct flight_node{
    flight_struct data;
    struct flight_node *next;
}flight_list_node;

typedef struct tower_node *tower_list;
typedef struct tower_node{
    tower_struct data;
    struct tower_node *next;
}tower_list_node;

typedef struct thread_node *thread_list;
typedef struct thread_node{
    pthread_t thread;
    int id;
    struct thread_node *next;
}thread_list_node;

typedef struct{
    //mainTime count
    int mainTime;
    int towerTime;
    int running;
    //cenas
    int currentArrival;
    int currentDepart;
    //inicial timespec
    struct timespec initialSpec;
    //linked list head
    flight_list head;
    tower_list towerHead;
    thread_list threadHead;
    //config data
    config_struct config;
    //ID's: threads, process, memory
    int shmid, shmid2, mq_id;
    shared_struct *shmem;
    pid_t tower;
    pthread_t time_thread;
    //condVar to shared use
    pthread_mutexattr_t mattr;
    pthread_condattr_t cattr;
    //mainTime conditional var
    cond_struct simulationManager;
    cond_struct controlTower;
    //file descriptor
    int named_fd;
    //semaphore
    sem_t *semLog, *semMQ, *semSHM, *semArrival, *semDepart;
    //File
    FILE *log;
}global_struct;

global_struct global;
//signals
void ignoreSignal(int signum){
    if(signum != SIGCHLD) printf("Signal received %d\n", signum);
}
void finit(){
    global.running = 0;
}
void sigint(int signum){
    printf(" pressed, exiting...\n");
    //wait for control tower
    int status;
    waitpid(global.tower, &status, 0);
    //wait for flight threads
    thread_list aux = global.threadHead -> next;
    while(aux){
        pthread_join(aux->thread, NULL);
        aux = aux -> next;
    }
    //Remove message queues
    msgctl(global.mq_id, IPC_RMID, 0);
    //Remove semaphores
    sem_unlink("SemLog");
    sem_unlink("SemMQ");
    sem_unlink("SemSHM");
    //Remove shared memory
    shmdt(&global.shmem->flight_slot);
    shmdt(&global.shmem);
    shmctl(global.shmid, IPC_RMID, NULL);
    shmctl(global.shmid2, IPC_RMID, NULL);
    //Close log.txt
    fclose(global.log);
    //Remove pipe
    unlink(PIPE_NAME);
    //exit
    exit(0);
}
void showStats(int signum){
    if (sem_wait(global.semSHM) != 0) {
        perror("ERROR WHILE DECREMENTING semSHM\n");
        exit(0);
    }
    printf("\n\nSIGUSR1 received! Showing stats\n\n");
    //Número total de voos criados
    printf("Número total de voos criados: %d\n", global.shmem -> num_created);
    //Número total de voos que descolaram
    printf("Número total de voos que descolaram: %d\n", global.shmem -> num_depart);
    //Número total de voos que aterraram
    printf("Número total de voos que aterraram: %d\n", global.shmem -> num_arrive);
    //Tempo médio de espera (para além do ETA) para aterrar
    printf("Tempo médio de espera (para além do ETA) para aterrar: %d\n", global.shmem -> wait_arrive / global.shmem->num_arrive);
    //Tempo médio de espera para descolar
    printf("Tempo médio de espera para descolar: %d\n", global.shmem -> wait_depart / global.shmem -> num_depart);
    //Número médio de manobras de holding por voo de aterragem
    printf("Número médio de manobras de holding por voo de aterragem: %d\n", global.shmem -> holding_arrive / global.shmem->num_arrive);
    //Número médio de manobras de holding por voo em estado de urgência
    printf("Número de vôos urgentes: %d\n", global.shmem -> urgence);
    //Número de voos redirecionados para outro aeroporto
    printf("Número de voos redirecionados para outro aeroporto: %d\n", global.shmem -> redirect);
    //Voos rejeitados pela Torre de Controlo
    printf("Voos rejeitados pela Torre de Controlo: %d\n", global.shmem -> rejected);
    fflush(stdout);
    if (sem_post(global.semSHM) != 0) {
        perror("ERROR WHILE DECREMENTING semSHM\n");
        exit(0);
    }
}
//write to log
void writeLog(char* message) {
    //ADD TIME
    char buffer[1024];
    time_t t = time(NULL);
    struct tm timeinfo = *localtime(&t);
    sprintf(buffer, "%d:%d:%d %s", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, message);
    //FUNCTION WRITE INTO LOG
    if (sem_wait(global.semLog) != 0) {
        perror("ERROR WHILE DECREMENTING semLog\n");
        exit(0);
    }
    fprintf(global.log, "%s", buffer);
    printf("%s\n", buffer);
    if (sem_post(global.semLog) != 0) {
        perror("ERROR WHILE INCREMENTING semLog\n");
        exit(0);
    }
}
//all flight linked list functions
flight_list createFlightList (void)
{
    flight_list aux;
    aux = (flight_list) malloc(sizeof(flight_list_node));
    if (aux != NULL) {
        aux -> data.mytype = -1;
        strcpy(aux -> data.flight_code, "HEAD");
        aux -> data.init = 0;
        aux -> data.takeoff = 0;
        aux -> data.eta = 0;
        aux -> data.fuel = 0;
        aux -> next = NULL;
    }
    return aux;
}
void printFlightList (flight_list head)
{
    flight_list aux = head -> next; /* Salta o header */
    printf("CURRENT LIST\n");
    while (aux)
    {
        printf("FLIGHT CODE: %s\n", aux -> data.flight_code);
        printf("TAKEOFF: %d\n", aux -> data.takeoff);
        printf("FUEL: %d\n", aux -> data.fuel);
        printf("ETA: %d\n", aux -> data.eta);
        printf("TYPE: %ld\n", aux -> data.mytype);
        printf("INIT: %d\n", aux -> data.init);
        aux = aux -> next;
    }
    printf("\n");
}
void searchFlightList (flight_list head, int num, flight_list *ant, flight_list *actual, int *pos)
{
    *ant = head; *actual = head->next;
    while ((*actual) != NULL && (*actual)-> data.init < num)
    {
        *ant = *actual;
        *actual = (*actual) -> next;
        pos++;
    }
    if ((*actual) != NULL && (*actual) -> data.init != num) {
        *actual = NULL; /* Se elemento não encontrado*/
        pos = 0;
    }
}
int insertFlightList (flight_list head, flight_list element)
{
    flight_list ant, inutil;
    int pos = 0;
    if (element != NULL) {
        searchFlightList (head, element -> data.init, &ant, &inutil, &pos);
        element -> next = ant -> next;
        ant -> next = element;
    }
    return pos;
}
void delFlightList (flight_list head, flight_list element)
{
    flight_list ant;
    flight_list actual;
    int inutil = 0;
    searchFlightList (head, element -> data.init, &ant, &actual, &inutil);
    if (actual != NULL) {
        ant -> next = actual -> next;
        free(actual);
    }
}

tower_list createTowerList (void)
{
    tower_list aux;
    aux = (tower_list) malloc(sizeof(tower_list_node));
    if (aux != NULL) {
        aux -> data.type = -1;
        strcpy(aux -> data.flight_code, "HEAD");
        aux -> data.slot = 0;
        aux -> data.type = 0;
        aux -> data.sendTime = 0;
        aux -> data.fuel = 0;
        aux -> next = NULL;
    }
    return aux;
}
void printTowerList (tower_list head)
{
    tower_list aux = head -> next; /* Salta o header */
    printf("CURRENT LIST\n");
    while (aux)
    {
        printf("FLIGHT CODE: %s\n", aux -> data.flight_code);
        printf("SLOT: %d\n", aux -> data.slot);
        printf("SEND TIME: %d\n", aux -> data.sendTime);
        printf("FUEL: %d\n", aux -> data.fuel);
        printf("TYPE: %d\n", aux -> data.type);
        aux = aux -> next;
    }
    printf("\n");
}
void searchTowerList (tower_list head, int sendTime, int fuel, tower_list *ant, tower_list *actual, int *pos)
{
    *ant = head; *actual = head->next;
    //FIND FIRST NODE WITH BIGGER sendTime
    while ((*actual) != NULL && (*actual)-> data.sendTime < sendTime)
    {
        *ant = *actual;
        *actual = (*actual) -> next;
        pos++;
    }
    while ((*actual) != NULL && (*actual)-> data.sendTime == sendTime && (*actual) -> data.fuel < fuel)
    {
        *ant = *actual;
        *actual = (*actual) -> next;
        pos++;
    }
    if ((*actual) != NULL && (*actual) -> data.sendTime != sendTime) {
        *actual = NULL; /* Se elemento não encontrado*/
        pos = 0;
    }
}
int insertTowerList (tower_list head, tower_list element)
{
    tower_list ant, inutil;
    int pos = 0;
    if (element != NULL) {
        searchTowerList (head, element -> data.sendTime, element -> data.fuel, &ant, &inutil, &pos);
        element -> next = ant -> next;
        ant -> next = element;
    }
    return pos;
}
void delTowerList (tower_list head, tower_list element)
{
    tower_list ant;
    tower_list actual;
    int inutil = 0;
    searchTowerList (head, element -> data.sendTime, element -> data.fuel ,&ant, &actual, &inutil);
    if (actual != NULL) {
        ant -> next = actual -> next;
        free(actual);
    }
}

thread_list createThreadList (void)
{
    thread_list aux;
    aux = (thread_list) malloc(sizeof(thread_list_node));
    if (aux != NULL) {
        aux -> id = 0;
        aux -> next = NULL;
    }
    return aux;
}
void printThreadList (thread_list head)
{
    thread_list aux = head -> next; /* Salta o header */
    printf("CURRENT THREADS\n");
    while (aux)
    {
        printf("Thread %d\n", aux -> id);
        aux = aux -> next;
    }
    printf("\n");
}
void searchThreadList (thread_list head, int num, thread_list *ant, thread_list *actual)
{
    *ant = head; *actual = head->next;
    while ((*actual) != NULL && (*actual) -> id < num)
    {
        *ant = *actual;
        *actual = (*actual) -> next;
    }
    if ((*actual) != NULL && (*actual) -> id != num) {
        *actual = NULL; /* Se elemento não encontrado*/
    }
}
void insertThreadList (thread_list head, thread_list element)
{
    thread_list ant, inutil;
    if (element != NULL) {
        searchThreadList (head, element -> id, &ant, &inutil);
        element -> next = ant -> next;
        ant -> next = element;
    }
}
void delThreadList (thread_list head, thread_list element)
{
    thread_list ant;
    thread_list actual;
    searchThreadList (head, element -> id, &ant, &actual);
    if (actual != NULL) {
        ant -> next = actual -> next;
        free(actual);
    }
}
//time functions
struct timespec addTime(struct timespec spec, int unit){
    //converter unit times into seconds and nanoseconds
    spec.tv_sec += unit * (global.config.time_unit/1000.0);
    spec.tv_nsec += (unit * 1000000000 / global.config.time_unit) % 1000000000;
    return spec;
}
int getTime(struct timespec spec){
    double timeSpent = (spec.tv_sec - global.initialSpec.tv_sec) + (spec.tv_nsec - global.initialSpec.tv_nsec) / BILLION;
    int time = timeSpent * (1000 / global.config.time_unit);
    return time;
}
//holding function
int getHolding(){
    return rand() % (global.config.hold_max - global.config.hold_min + 1);
}
//control tower
void *flightManager(){
    int delta = 0;
    struct timespec spec;
    while(1){
        //get the actual program time
        clock_gettime(CLOCK_REALTIME, &spec);
        global.towerTime = getTime(spec);
        delta = global.towerTime;
        //if linked list is null
        while(global.towerHead -> next == NULL) {
            if(global.running){
                pthread_cond_wait(&global.controlTower.condVar, &global.controlTower.mutex);
                //update the time
                clock_gettime(CLOCK_REALTIME, &spec);
                global.towerTime = getTime(spec);
            }
            else{
                pthread_exit(NULL);
            }
        }
        spec = addTime(global.initialSpec, global.towerHead -> next -> data.sendTime);
        pthread_cond_timedwait(&global.controlTower.condVar, &global.controlTower.mutex, &spec);
        //update the time
        clock_gettime(CLOCK_REALTIME, &spec);
        global.towerTime = getTime(spec);
        spec = addTime(spec, global.towerHead -> next -> data.sendTime);
        //send send autorization to all flights with right sendTime
        //shared memory semaphore
        if (sem_wait(global.semSHM) != 0) {
            perror("ERROR WHILE DECREMENTING semSHM\n");
            exit(0);
        }
        while(global.towerHead -> next -> data.sendTime == global.towerTime){
            //send the first flight in tower queue
            int i = 1;
            //mutex lock
            pthread_mutex_lock(&(global.shmem -> flight_slot + global.towerHead -> next -> data.slot) -> cond.mutex);
            (global.shmem -> flight_slot + global.towerHead -> next -> data.slot) -> holding = 0;
            (global.shmem -> flight_slot + global.towerHead -> next -> data.slot) -> autorization = 1;
            //signal changes
            pthread_cond_signal(&(global.shmem -> flight_slot + global.towerHead -> next -> data.slot) -> cond.condVar);
            //unlock mutex
            pthread_mutex_unlock(&(global.shmem -> flight_slot + global.towerHead -> next -> data.slot) -> cond.mutex);
            //verificar se o próximo é do mesmo tipo
            if((global.towerHead -> next -> data.type) == (global.towerHead -> next -> next -> data.type)){
                //mutex lock
                pthread_mutex_lock(&(global.shmem -> flight_slot + global.towerHead -> next -> next -> data.slot) -> cond.mutex);
                (global.shmem -> flight_slot + global.towerHead -> next -> next -> data.slot) -> holding = 0;
                (global.shmem -> flight_slot + global.towerHead -> next -> next -> data.slot) -> autorization = 1;
                //signal changes
                pthread_cond_signal(&(global.shmem -> flight_slot + global.towerHead -> next -> next -> data.slot) -> cond.condVar);
                //unlock mutex
                pthread_mutex_unlock(&(global.shmem -> flight_slot + global.towerHead -> next -> next -> data.slot) -> cond.mutex);
                i++;
            }
            //wait arrive or depart time
            switch(global.towerHead -> next -> data.type){
                case 2:
                    usleep(1000 * global.config.time_unit(global.config.delta_arrive + global.config.max_arrive));
                    //numero de arrivals
                    global.shmem->num_arrive++;
                    break;
                case 3:
                    //tempo de espera de todos os departs
                    global.shmem -> wait_depart += global.towerTime - (global.towerHead -> next -> data.sendTime);
                    usleep(1000 * global.config.time_unit(global.config.delta_depart + global.config.depart));
                    //numero de departs
                    global.shmem->num_depart++;
                    break;
            }
            //remove from queue
            int j;
            for(j = 0; j < i; j++){
                //remove from linked list
                delTowerList(global.towerHead, global.towerHead -> next);
                //remove shared memory slot
                pthread_cond_destroy(&((global.shmem -> flight_slot + global.towerHead -> next -> data.slot) -> cond.condVar));
                pthread_mutex_destroy(&((global.shmem -> flight_slot + global.towerHead -> next -> data.slot) -> cond.mutex));
                //null slots
                (global.shmem -> flight_slot + global.towerHead -> next -> data.slot) -> holding = -1;
            }
        }
        //update time
        clock_gettime(CLOCK_REALTIME, &spec);
        global.towerTime = getTime(spec);
        delta = delta - global.towerTime;
        //verify if next flights needs hold or need to be redirected
        tower_list tower = global.towerHead -> next;
        while(tower != NULL){
            //updates fuel
            (global.shmem -> flight_slot + tower -> data.slot) -> fuel -= delta;
            //verify if flights hold until next departure/arrival
            if(tower -> data.type == 2 && tower -> data.fuel <= (tower -> data.sendTime + global.config.delta_arrive + global.config.arrive) * global.config.time_unit){
                (global.shmem -> flight_slot + tower -> data.slot) -> holding = -1;
                //notify flight to check updates
                pthread_cond_signal(&((global.shmem -> flight_slot + tower -> data.slot) -> cond.condVar));
            }else {
                //adicionar holding se necessário
                if(tower -> data.sendTime + (global.shmem -> flight_slot + tower -> data.slot) -> holding < global.towerTime){
                    int aux = getHolding();
                    (global.shmem -> flight_slot + tower -> data.slot) -> holding = aux;
                    //holdings totais
                    global.shmem -> wait_arrive += aux;
                    //numero de holdings
                    global.shmem -> holding_arrive++;
                }
            }
            tower = tower -> next;
            //decrement shared memory semaphore
            if (sem_post(global.semSHM) != 0) {
                perror("ERROR WHILE DECREMENTING semSHM\n");
                exit(0);
            }
        }
    }
    return 0;
}
int controlTower() {
    //redirect signal
    signal(SIGINT, finit);
    signal(SIGUSR1, showStats);
    //ignore other signals
    for(int i=1; i<31; i++){
        if((i!=2) && (i!=10) && (i!=11)){
            signal(i, ignoreSignal);
        }
    }
    //inicialize tower condVar
    pthread_cond_init(&global.controlTower.condVar, &global.cattr);
    pthread_mutex_init(&global.controlTower.mutex, &global.mattr);
    global.towerHead = createTowerList();
    //Global List
    pthread_t managerThread;
    flight_struct flight;
    global.currentArrival = 0;
    global.currentDepart = 0;
    slot_struct send;
    send.mytype = 4;
    //create thread
    pthread_create(&managerThread, NULL, flightManager, NULL);
    //receive message queues
    while (global.running) {
        //wait to receive message from message queue
        msgrcv(global.mq_id, &flight, sizeof(flight_struct) - sizeof(long), -3, 0);
        /*
        printf("RECEIVED: %s\n", flight.flight_code);
        printf("ETA: %d\n", flight.eta);
        printf("FUEL: %d\n", flight.fuel);
        printf("TAKEOFF: %d\n", flight.takeoff);
        printf("INIT: %d\n", flight.init);
        printf("TYPE: %ld\n", flight.mytype);
        */
        //aumentar o numero de voos criados na shared memory
        if (sem_wait(global.semSHM) != 0) {
            perror("ERROR WHILE DECREMENTING semSHM\n");
            exit(0);
        }
        //numero de voos criados
        global.shmem -> num_created++;
        //search avaliable shared memory slots
        int i;
        for (i = 0; i < global.config.max_depart + global.config.max_arrive; i++) {
            if ((global.shmem -> flight_slot + i) -> holding == -1) {
                break;
            }
        }
        send.slot = i;
        if (flight.mytype == 3 && global.currentDepart < global.config.max_depart) {
            tower_list node = (tower_list) malloc(sizeof(tower_list_node));
            strcpy(node->data.flight_code, flight.flight_code);
            node->data.type = DEPART;
            node->data.slot = i;
            node->data.sendTime = flight.takeoff;
            node->data.fuel = INT32_MAX;
            pthread_mutex_lock(&global.controlTower.mutex);
            pthread_cond_init(&((global.shmem -> flight_slot + i) -> cond.condVar), &global.cattr);
            pthread_mutex_init(&((global.shmem -> flight_slot + i) -> cond.mutex), &global.mattr);
            (global.shmem -> flight_slot + i) -> holding = 0;
            (global.shmem -> flight_slot + i) -> autorization = 0;
            insertTowerList(global.towerHead, node);
            global.currentDepart++;
            pthread_cond_signal(&global.controlTower.condVar);
            pthread_mutex_unlock(&global.controlTower.mutex);
        } else if (flight.mytype == 2 && global.currentArrival < global.config.max_arrive) {
            tower_list node = (tower_list) malloc(sizeof(tower_list_node));
            strcpy(node->data.flight_code, flight.flight_code);
            node->data.type = ARRIVE;
            node->data.slot = i;
            node->data.fuel = flight.fuel;
            node->data.sendTime = flight.eta + flight.init;
            //lock semaphore
            pthread_mutex_lock(&global.controlTower.mutex);
            //inicializa shared memory zone
            pthread_cond_init(&((global.shmem -> flight_slot + i) -> cond.condVar), &global.cattr);
            pthread_mutex_init(&((global.shmem -> flight_slot + i) -> cond.mutex), &global.mattr);
            (global.shmem -> flight_slot + i) -> holding = 0;
            (global.shmem -> flight_slot + i) -> autorization = 0;
            //insert in linked list and verify if it has a lower sendTime than expected
            insertTowerList(global.towerHead, node);
            global.currentArrival++;
            pthread_cond_signal(&global.controlTower.condVar);
            pthread_mutex_unlock(&global.controlTower.mutex);
        }
        else{
            send.slot = -1;
            //numero de voos rejeitados
            global.shmem -> rejected++;
        }
        //shared memory semaphore
        if (sem_post(global.semSHM) != 0) {
            perror("ERROR WHILE DECREMENTING semSHM\n");
            exit(0);
        }
        //send message with: shared memory slot
        if (msgsnd(global.mq_id, &send, sizeof(slot_struct) - sizeof(long), -2) < 0) {
            perror("MESSAGE NOT SENT\n");
            exit(0);
        }
        pthread_mutex_unlock(&global.controlTower.mutex);
    }
    pthread_join(managerThread, NULL);
    exit(0);
}
//flight threads
void *departureFlight(void *info){
    //INÍCIO
    flight_struct* flight = (flight_struct*)info;
    char buffer[BUF_SIZE];
    slot_struct receive;
    //PRINT
    sprintf(buffer, "DEPARTURE %s STARTED\n", flight -> flight_code);
    writeLog(buffer);
    //SEND FLIGHT INFO TO CONTROL TOWER
    if(msgsnd(global.mq_id, flight, sizeof(flight_struct) - sizeof(long), 0) < 0){
        perror("MESSAGE NOT SENT\n");
        exit(0);
    }
    //RECEIVE SHARED MEMORY SLOT
    msgrcv(global.mq_id, &receive, sizeof(slot_struct) - sizeof(long), 4, 0);
    //LOCK MUTEX
    pthread_mutex_lock(&((global.shmem->flight_slot + receive.slot) -> cond.mutex));
    //WAIT FOR AUTORIZATION
    while((global.shmem->flight_slot + receive.slot) -> autorization != 1){
        pthread_cond_wait(&((global.shmem->flight_slot + receive.slot) -> cond.condVar), &((global.shmem->flight_slot + receive.slot) -> cond.mutex));
    }
    //WAIT FOR HOLDING
    while((global.shmem->flight_slot + receive.slot) -> holding > 0){
        pthread_cond_wait(&((global.shmem->flight_slot + receive.slot) -> cond.condVar), &((global.shmem->flight_slot + receive.slot) -> cond.mutex));
    }
    pthread_mutex_unlock(&((global.shmem->flight_slot + receive.slot) -> cond.mutex));
    //TAKE ONE PISTA
    usleep(1000 * global.config.time_unit * global.config.depart);
    //PRINT
    sprintf(buffer, "DEPARTURE %s CONCLUDED\n", flight -> flight_code);
    writeLog(buffer);
    pthread_exit(NULL);
}
void *arrivalFlight(void *info){
    flight_struct* flight = (flight_struct*)info;
    char buffer[BUF_SIZE];
    slot_struct receive;
    //imprimir info
    sprintf(buffer, "LANDING %s STARTED\n", flight -> flight_code);
    writeLog(buffer);
    //VERIFY IT IT IS A PRIORITY FLIGHT
    if(flight -> fuel == 4 + flight -> eta + global.config.delta_arrive){
        flight -> mytype = PRIORITY_ARRIVE;
        if (sem_wait(global.semSHM) != 0) {
            perror("ERROR WHILE DECREMENTING semSHM\n");
            exit(0);
        }
        global.shmem->urgence++;
        if (sem_post(global.semSHM) != 0) {
            perror("ERROR WHILE INCREMENTING semSHM\n");
            exit(0);
        }
    }
    //SEND FLIGHT INFORMATION
    if(msgsnd(global.mq_id, flight, sizeof(flight_struct) - sizeof(long), 0) < 0){
        perror("MESSAGE NOT SENT\n");
        exit(0);
    }
    //RECEIVE SHARED MEMORY SLOT
    msgrcv(global.mq_id, &receive, sizeof(receive), 4, 0);
    //REJECTED
    if(receive.slot == -1){
        sprintf(buffer, "ARRIVAL %s REJECTED\n", flight -> flight_code);
        writeLog(buffer);
        pthread_exit(NULL);
    }
    //WAIT FOR AUTORIZATION
    pthread_mutex_lock(&((global.shmem->flight_slot + receive.slot) -> cond.mutex));
    while((global.shmem->flight_slot + receive.slot) -> autorization != 1){
        pthread_cond_wait(&((global.shmem->flight_slot + receive.slot) -> cond.condVar), &((global.shmem->flight_slot + receive.slot) -> cond.mutex));
    }
    pthread_mutex_unlock(&((global.shmem->flight_slot + receive.slot) -> cond.mutex));
    //WAIT FOR HOLDING
    pthread_mutex_lock(&((global.shmem->flight_slot + receive.slot) -> cond.mutex));
    while((global.shmem->flight_slot + receive.slot) -> holding > 0){
        pthread_cond_wait(&((global.shmem->flight_slot + receive.slot) -> cond.condVar), &((global.shmem->flight_slot + receive.slot) -> cond.mutex));
    }
    pthread_mutex_unlock(&((global.shmem->flight_slot + receive.slot) -> cond.mutex));
    //REDIRECTED
    if((global.shmem -> flight_slot + receive.slot) -> holding == -1){
        sprintf(buffer, "%s LEAVING TO OTHER AIRPORT => FUEL = %d\n", flight -> flight_code, flight -> fuel);
        writeLog(buffer);
        pthread_exit(NULL);
    }
    usleep(1000 * global.config.time_unit * global.config.arrive);
    //imprimir info
    sprintf(buffer, "LANDING %s CONCLUDED\n", flight -> flight_code);
    writeLog(buffer);
    //finish
    pthread_exit(NULL);

}
void *timeThread(){
    int i = 1;
    struct timespec spec;
    while(1){
        /* TEMPO USANDO SLEEP
        usleep(global.config.time_unit * 1000);
        global.mainTime++;
        printf("Current mainTime: %d\n", global.mainTime);
        printFlightList(global.head)*/
        //lock mutex
        pthread_mutex_lock(&global.simulationManager.mutex);
        //get current timespec
        clock_gettime(CLOCK_REALTIME, &spec);
        //update current mainTime
        global.mainTime = getTime(spec);
        //if linked list is null
        if(global.head -> next == NULL) {
            pthread_cond_wait(&global.simulationManager.condVar, &global.simulationManager.mutex);
        }
        //add waiting time
        spec = addTime(global.initialSpec,  global.head -> next -> data.init);
        pthread_cond_timedwait(&global.simulationManager.condVar, &global.simulationManager.mutex, &spec);
        //update current mainTime
        clock_gettime(CLOCK_REALTIME, &spec);
        global.mainTime = getTime(spec);
        flight_struct flight;
        while((global.head -> next) != NULL && (global.head -> next) -> data.init <= global.mainTime){
            //copy info into struct
            strcpy(flight.flight_code, global.head->next->data.flight_code);
            flight.takeoff = global.head->next->data.takeoff;
            flight.eta = global.head->next->data.eta;
            flight.mytype = global.head->next->data.mytype;
            flight.init = global.head->next->data.init;
            flight.fuel = global.head->next->data.fuel;
            //se é departure
            thread_list thread;
            thread = (thread_list) malloc(sizeof(thread_list_node));
            thread -> id = i;
            long aux = DEPART;
            long aux2 = ARRIVE;
            if(global.head -> next -> data.mytype == aux){
                //create flight thread
                flight.eta = 0;
                flight.fuel = INT8_MAX;
                pthread_create(&thread->thread, NULL, departureFlight, (void*)&flight);
                insertThreadList(global.threadHead, thread);
            }
                //se é arrival
            else if(global.head -> next -> data.mytype == aux2){
                //create flight thread
                flight.takeoff = 0;
                pthread_create(&thread->thread, NULL, arrivalFlight, (void*)&flight);
            }
            //elimina nó da linked list
            delFlightList(global.head, global.head->next);
        }
        pthread_mutex_unlock(&global.simulationManager.mutex);
    }
}
//startup
config_struct arranque(){
    FILE *f;
    config_struct config;
    char buffer[9][10];
    char token;
    int j = 0;

    f = fopen("config.txt", "r");
    int i = 0;
    while(!feof(f)){
        token = fgetc(f);
        if(token != '\n' && token != ','){
            buffer[i][j++] = token;
        }
        else if(token == ','){
            fgetc(f);
            buffer[i++][j]='\0';
            j = 0;
        }
        else{
            buffer[i++][j]='\0';
            j = 0;
        }
    }
    config.time_unit = atoi(buffer[0]);
    config.depart = atoi(buffer[1]);
    config.delta_depart = atoi(buffer[2]);
    config.arrive = atoi(buffer[3]);
    config.delta_arrive = atoi(buffer[4]);
    config.hold_min = atoi(buffer[5]);
    config.hold_max = atoi(buffer[6]);
    config.max_depart = atoi(buffer[7]);
    config.max_arrive = atoi(buffer[8]);
    return config;
}
//comand check
flight_struct checkCommand(char *buffer){
    //FUNCTION CHECKS COMMAND: RETURNS COMMAND STRUCT: .validity = 1 if command depart; .validity = 2 if command arrive; .validity = 0 if command not valid
    int check;
    char aux [5][255];
    //get actual time
    struct timespec spec;
    clock_gettime(CLOCK_REALTIME, &spec);
    int time = getTime(spec);
    //
    flight_struct comand;
    check = sscanf(buffer, "%s %s %s %d %s %d %s %d %s", aux[0], comand.flight_code, aux[1], &comand.init, aux[2], &comand.eta, aux[3], &comand.fuel, aux[4]);
    //VERIFY IF IS A DEPARTURE COMMAND
    if(check == 6){
        if(strcmp(aux[0], "DEPARTURE") == 0 && strcmp(aux[1], "init:") == 0 && strcmp(aux[2], "takeoff:") == 0 && comand.init > time && comand.eta > comand.init){
            comand.takeoff = comand.eta;
            comand.mytype = DEPART;
            return comand;
        }
    }
        //VERIFY IF IT IS AN ARRIVE COMMAND
    else if(check == 8){
        if(strcmp(aux[0], "ARRIVAL") == 0 && strcmp(aux[1], "init:") == 0 && strcmp(aux[2], "eta:") == 0 && strcmp(aux[3], "fuel:") == 0 && comand.init > time && comand.fuel > comand.eta){
            comand.mytype = ARRIVE;
            return comand;
        }
    }
    //IT IS NOT A VALID COMMAND
    comand.mytype = 0;
    return comand;
}
//main
int main()
{
    //redirect signal
    signal(SIGINT, sigint);
    //ignore other signals
    for(int i=1; i<31; i++){
        if((i!=2) && (i!=10) && (i!=11)){
            signal(i, ignoreSignal);
        }
    }
    //
    global.running = 1;
    //get initial spec
    clock_gettime(CLOCK_REALTIME, &global.initialSpec);
    //inicialize random
    srand(time(NULL));
    //create linked list
    global.head = createFlightList();
    global.threadHead = createThreadList();
    //Open log.txt
    global.log = fopen("log.txt", "a");
    //read data from file
    global.config = arranque();
    //create shared memory zones
    //alocate to *shmem
    if((global.shmid = shmget(IPC_PRIVATE, sizeof(shared_struct), IPC_CREAT | 0666)) < 0){
        perror("ERROR CREATING SHARED MEMORY\n");
        exit(-1);
    }
    if((global.shmem = shmat(global.shmid, NULL, 0)) < 0){
        perror("ERROR ATTACHING SHARED MEMORY ZONE\n");
        exit(-1);
    }
    //allocate to shmem -> flight_slot
    if((global.shmid2 = shmget(IPC_PRIVATE, (global.config.max_arrive + global.config.max_depart) * sizeof(tower_queue), IPC_CREAT | 0666)) < 0){
        perror("ERROR CREATING SHARED MEMORY\n");
        exit(-1);
    }
    if((global.shmem -> flight_slot = shmat(global.shmid2, NULL, 0)) < 0){
        perror("ERROR ATTACHING SHARED MEMORY ZONE\n");
        exit(-1);
    }
    //inicializa shared memory spaces
    global.shmem -> num_created = 0;     //Número total de voos criados
    global.shmem ->  num_depart = 0;     //Número total de voos que descolaram
    global.shmem ->  num_arrive = 0;     //Número total de voos que aterraram
    global.shmem ->  wait_arrive = 0;    //Tempo médio de espera (para além do ETA) para aterrar
    global.shmem ->  wait_depart = 0;    //Tempo médio de espera para descolar
    global.shmem ->  holding_arrive = 0; //Número médio de manobras de holding por voo de aterragem
    global.shmem ->  urgence = 0;//Número médio de manobras de holding por voo em estado de urgência
    global.shmem ->  redirect = 0;       //Número de voos redirecionados para outro aeroporto
    global.shmem ->  rejected = 0;       //Voos rejeitados pela Torre de Controlo
    int i;
    for(i = 0; i < global.config.max_arrive + global.config.max_depart; i++){
        (global.shmem -> flight_slot + i) -> holding = -1;
    }
    //inicialize mutex and cond var to conditional vars
    pthread_mutexattr_setpshared(&global.mattr, PTHREAD_PROCESS_SHARED);
    pthread_condattr_setpshared(&global.cattr, PTHREAD_PROCESS_SHARED);
    //create mainTime conditional vars
    pthread_cond_init(&global.simulationManager.condVar, &global.cattr);
    pthread_mutex_init(&global.simulationManager.mutex, &global.mattr);
    //unlink input pipe
    unlink(PIPE_NAME);
    //create and open pipe
    if(mkfifo(PIPE_NAME, O_CREAT|O_EXCL|0700) < 0){
        perror("ERROR WHILE CREATING PIPE\n");
        exit(-1);
    }
    if((global.named_fd = open(PIPE_NAME, O_RDWR)) < 0){
        perror("ERROR WHILE OPENING PIPE\n");
        exit(-1);
    }
    //create message queue
    if((global.mq_id = msgget(IPC_PRIVATE, O_CREAT|0700)) < 0){
        perror("ERROR CREATING MESSAGE QUEUE\n");
        exit(-1);
    }
    //unlink previous semaphores
    sem_unlink("SemLog");
    sem_unlink("SemSHM");
    //initialize semaphores
    if((global.semLog = sem_open("SemLog", O_CREAT|O_EXCL, 0600, 1)) == SEM_FAILED){
        perror("ERROR INICIALIZE SemLog\n");
        exit(-1);
    }
    if((global.semSHM = sem_open("SemSHM", O_CREAT, 0600, 1)) == SEM_FAILED){
        perror("ERROR INICIALIZE SemSHM\n");
        exit(-1);
    }
    //create controlTower process
    if(fork() == 0){
        global.tower = getpid();
        controlTower();
        exit(0);
    }
    //start mainTime thread
    if (pthread_create (&global.time_thread, NULL, timeThread, NULL) != 0)
    {
        perror("ERROR CREATING TIME THREAD\n");
        exit(-1);
    }
    int nread;
    char buffer[BUF_SIZE];
    char *info;
    info = (char*)malloc(BUF_SIZE);
    //Write to log the start of the program
    sprintf(buffer, "START\n");
    writeLog(buffer);
    //read from pipe
    while(global.running){
        //create an linked list struct
        //READS STRINGS FROM PIPE AND PUT AN '\0' AT END OF IT
        while((nread = read(global.named_fd, buffer, BUF_SIZE)) < 1);
        buffer[nread-1] = '\0';
        flight_struct flight = checkCommand(buffer);
        memset(info, 0, BUF_SIZE);
        //depart or arrive command
        if(flight.mytype > 0){
            sprintf(info, "NEW COMMAND => %s\n", buffer);
            writeLog(info);
            flight_list node;
            node = (flight_list)malloc(sizeof(flight_list_node));
            node -> data = flight;
            //
            insertFlightList(global.head, node);
            //ORDENAR OBSERVAR
            pthread_cond_signal(&global.simulationManager.condVar);
        }
            //wrong command
        else{
            sprintf(info, "WRONG COMMAND => %s\n", buffer);
            writeLog(info);
        }
    }
    return 0;
}
