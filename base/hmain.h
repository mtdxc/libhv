#ifndef HV_MAIN_H_
#define HV_MAIN_H_

#include "hexport.h"
#include "hplatform.h"
#include "hdef.h"
#include "hproc.h"

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib") // for timeSetEvent
#endif

BEGIN_EXTERN_C

typedef int (*printf_t)(const char *const fmt, ...);

typedef struct main_ctx_s {
    char    run_dir[MAX_PATH];
    char    program_name[MAX_PATH]; // program filename without ext

    char    confile[MAX_PATH]; // default etc/${program}.conf
    char    pidfile[MAX_PATH]; // default logs/${program}.pid
    char    logfile[MAX_PATH]; // default logs/${program}.log

    pid_t   pid;    // getpid
    pid_t   oldpid; // getpid_from_pidfile

    // arg
    int     argc; // count
    int     arg_len; // cmdline length
    char**  os_argv; // pass by system, end with NULL
    char**  save_argv; // copy by main_ctx_init, end with NULL
    char*   cmdline; // cmdline str   
    // parsed arg
    int     arg_kv_size;
    char**  arg_kv; // arg_kv[idx]="key=val"
    int     arg_list_size;
    char**  arg_list; // arg_list[idx] = str

    // env
    int     envc; // count
    int     env_len; // all env str length
    char**  os_envp; // pass by system
    char**  save_envp; // copy by main_ctx_init, end with NULL

    // signals
    procedure_t     reload_fn;
    void*           reload_userdata;
    // master workers model
    int             worker_processes;
    int             worker_threads;
    procedure_t     worker_fn;
    void*           worker_userdata;
    proc_ctx_t*     proc_ctxs;
} main_ctx_t;

// arg_type
#define NO_ARGUMENT         0
#define REQUIRED_ARGUMENT   1
#define OPTIONAL_ARGUMENT   2
// option define
#define OPTION_PREFIX   '-'
#define OPTION_DELIM    '='
#define OPTION_ENABLE   "1"
#define OPTION_DISABLE  "0"
typedef struct option_s {
    char        short_opt;
    const char* long_opt;
    int         arg_type;
    const char* description;
} option_t;

HV_EXPORT int  main_ctx_init(int argc, char** argv);
HV_EXPORT void main_ctx_free(void);

// ls -a -l
// ls -al
// watch -n 10 ls
// watch -n10 ls
HV_EXPORT int parse_opt(int argc, char** argv, const char* opt);
// gcc -g -Wall -O3 -std=cpp main.c
HV_EXPORT int parse_opt_long(int argc, char** argv, const option_t* long_options, int opt_size);
HV_EXPORT int dump_opt_long(const option_t* long_options, int opt_size, char* out_str, int out_size);
HV_EXPORT const char* get_arg(const char* key);
HV_EXPORT const char* get_env(const char* key);

#if defined(OS_UNIX) && !HAVE_SETPROCTITLE
HV_EXPORT void setproctitle(const char* fmt, ...);
#endif

// pidfile
HV_EXPORT int   create_pidfile();
HV_EXPORT void  delete_pidfile(void);
HV_EXPORT pid_t getpid_from_pidfile();

// signal=[start,stop,restart,status,reload]
HV_EXPORT int  signal_init(procedure_t reload_fn DEFAULT(NULL), void* reload_userdata DEFAULT(NULL));
HV_EXPORT void signal_handle(const char* signal);
HV_EXPORT bool signal_handle_noexit(const char* signal);
#ifdef OS_UNIX
// we use SIGTERM to quit process, SIGUSR1 to reload confile
#define SIGNAL_TERMINATE    SIGTERM
#define SIGNAL_RELOAD       SIGUSR1
void signal_handler(int signo);
#endif

// global var
#define DEFAULT_WORKER_PROCESSES    4
#define MAXNUM_WORKER_PROCESSES     256
HV_EXPORT extern main_ctx_t   g_main_ctx;
HV_EXPORT extern printf_t     printf_fn;

// master-workers processes
HV_EXPORT int master_workers_run(
        procedure_t worker_fn,
        void* worker_userdata DEFAULT(NULL),
        int worker_processes DEFAULT(DEFAULT_WORKER_PROCESSES),
        int worker_threads DEFAULT(0),
        bool wait DEFAULT(true));

END_EXTERN_C

#endif // HV_MAIN_H_
