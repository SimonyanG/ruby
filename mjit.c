/**********************************************************************

  mjit.c - Interface to MRI method JIT compiler for Ruby's main thread

  Copyright (C) 2017 Vladimir Makarov <vmakarov@redhat.com>.

**********************************************************************/

/* Functions in this file are never executed on MJIT worker thread.
   So you can safely use Ruby methods and GC in this file. */

#ifdef __sun
#define __EXTENSIONS__ 1
#endif

#include "internal.h"
#include "vm_core.h"
#include "mjit.h"
#include "gc.h"
#include "constant.h"
#include "id_table.h"
#include "ruby_assert.h"
#include "ruby/thread.h"
#include "ruby/util.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <sys/time.h>
#include <dlfcn.h>
#endif
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif

#include "dln.h"
#include "mjit_internal.h"

extern int rb_thread_create_mjit_thread(void (*worker_func)(void));

/* A copy of MJIT portion of MRI options since MJIT initialization.  We
   need them as MJIT threads still can work when the most MRI data were
   freed. */
struct mjit_options mjit_opts;

/* TRUE if MJIT is enabled.  */
int mjit_enabled = FALSE;
/* TRUE if JIT-ed code should be called. When `ruby_vm_event_enabled_flags & ISEQ_TRACE_EVENTS`
   and `mjit_call_p == FALSE`, any JIT-ed code execution is cancelled as soon as possible. */
int mjit_call_p = FALSE;

/* Priority queue of iseqs waiting for JIT compilation.
   This variable is a pointer to head unit of the queue. */
struct rb_mjit_unit_list mjit_unit_queue;
/* List of units which are successfully compiled. */
struct rb_mjit_unit_list mjit_active_units;
/* List of compacted so files which will be deleted in `mjit_finish()`. */
struct rb_mjit_unit_list mjit_compact_units;
/* The number of so far processed ISEQs, used to generate unique id.  */
int mjit_current_unit_num;
/* A mutex for conitionals and critical sections.  */
rb_nativethread_lock_t mjit_engine_mutex;
/* A thread conditional to wake up `mjit_finish` at the end of PCH thread.  */
rb_nativethread_cond_t mjit_pch_wakeup;
/* A thread conditional to wake up the client if there is a change in
   executed unit status.  */
rb_nativethread_cond_t mjit_client_wakeup;
/* A thread conditional to wake up a worker if there we have something
   to add or we need to stop MJIT engine.  */
rb_nativethread_cond_t mjit_worker_wakeup;
/* A thread conditional to wake up workers if at the end of GC.  */
rb_nativethread_cond_t mjit_gc_wakeup;
/* True when GC is working.  */
int mjit_in_gc;
/* True when JIT is working.  */
int mjit_in_jit;

/* Path of "/tmp", which can be changed to $TMP in MinGW. */
char *mjit_tmp_dir;
/* Hash like { 1 => true, 2 => true, ... } whose keys are valid `class_serial`s.
   This is used to invalidate obsoleted CALL_CACHE. */
VALUE mjit_valid_class_serials;

extern const char *mjit_cc_path;
extern char *mjit_pch_file;

#ifndef _MSC_VER
/* Name of the header file.  */
extern char *mjit_header_file;
#endif

#ifdef _WIN32
/* Linker option to enable libruby. */
extern char *mjit_libruby_pathflag;
#endif

/* Make and return copy of STR in the heap. */
#define get_string ruby_strdup

/* Return an unique file name in /tmp with PREFIX and SUFFIX and
   number ID.  Use getpid if ID == 0.  The return file name exists
   until the next function call.  */
static char *
get_uniq_filename(unsigned long id, const char *prefix, const char *suffix)
{
    char buff[70], *str = buff;
    int size = sprint_uniq_filename(buff, sizeof(buff), id, prefix, suffix);
    str = 0;
    ++size;
    str = xmalloc(size);
    if (size <= (int)sizeof(buff)) {
        memcpy(str, buff, size);
    }
    else {
        sprint_uniq_filename(str, size, id, prefix, suffix);
    }
    return str;
}

/* Wait until workers don't compile any iseq.  It is called at the
   start of GC.  */
void
mjit_gc_start_hook(void)
{
    if (!mjit_enabled)
        return;
    CRITICAL_SECTION_START(4, "mjit_gc_start_hook");
    while (mjit_in_jit) {
        verbose(4, "Waiting wakeup from a worker for GC");
        rb_native_cond_wait(&mjit_client_wakeup, &mjit_engine_mutex);
        verbose(4, "Getting wakeup from a worker for GC");
    }
    mjit_in_gc = TRUE;
    CRITICAL_SECTION_FINISH(4, "mjit_gc_start_hook");
}

/* Send a signal to workers to continue iseq compilations.  It is
   called at the end of GC.  */
void
mjit_gc_finish_hook(void)
{
    if (!mjit_enabled)
        return;
    CRITICAL_SECTION_START(4, "mjit_gc_finish_hook");
    mjit_in_gc = FALSE;
    verbose(4, "Sending wakeup signal to workers after GC");
    rb_native_cond_broadcast(&mjit_gc_wakeup);
    CRITICAL_SECTION_FINISH(4, "mjit_gc_finish_hook");
}

/* Iseqs can be garbage collected.  This function should call when it
   happens.  It removes iseq from the unit.  */
void
mjit_free_iseq(const rb_iseq_t *iseq)
{
    if (!mjit_enabled)
        return;
    CRITICAL_SECTION_START(4, "mjit_free_iseq");
    if (iseq->body->jit_unit) {
        /* jit_unit is not freed here because it may be referred by multiple
           lists of units. `get_from_list` and `mjit_finish` do the job. */
        iseq->body->jit_unit->iseq = NULL;
    }
    CRITICAL_SECTION_FINISH(4, "mjit_free_iseq");
}

/* Do we need this...? */
static void
init_list(struct rb_mjit_unit_list *list)
{
    list->head = NULL;
    list->length = 0;
}

/* Free unit list. This should be called only when worker is finished
   because node of mjit_unit_queue and one of mjit_active_units may have the same unit
   during proceeding unit. */
static void
free_list(struct rb_mjit_unit_list *list)
{
    struct rb_mjit_unit_node *node, *next;
    for (node = list->head; node != NULL; node = next) {
        next = node->next;
        free_unit(node->unit);
        xfree(node);
    }
}

extern enum pch_status_t mjit_pch_status;
extern int mjit_stop_worker_p;
extern int mjit_worker_stopped;

/* MJIT info related to an existing continutaion.  */
struct mjit_cont {
    rb_execution_context_t *ec; /* continuation ec */
    struct mjit_cont *prev, *next; /* used to form lists */
};

/* Double linked list of registered continuations. This is used to detect
   units which are in use in unload_units. */
static struct mjit_cont *first_cont;

/* Register a new continuation with thread TH.  Return MJIT info about
   the continuation.  */
struct mjit_cont *
mjit_cont_new(rb_execution_context_t *ec)
{
    struct mjit_cont *cont;

    cont = ZALLOC(struct mjit_cont);
    cont->ec = ec;

    CRITICAL_SECTION_START(3, "in mjit_cont_new");
    if (first_cont == NULL) {
        cont->next = cont->prev = NULL;
    }
    else {
        cont->prev = NULL;
        cont->next = first_cont;
        first_cont->prev = cont;
    }
    first_cont = cont;
    CRITICAL_SECTION_FINISH(3, "in mjit_cont_new");

    return cont;
}

/* Unregister continuation CONT.  */
void
mjit_cont_free(struct mjit_cont *cont)
{
    CRITICAL_SECTION_START(3, "in mjit_cont_new");
    if (cont == first_cont) {
        first_cont = cont->next;
        if (first_cont != NULL)
            first_cont->prev = NULL;
    }
    else {
        cont->prev->next = cont->next;
        if (cont->next != NULL)
            cont->next->prev = cont->prev;
    }
    CRITICAL_SECTION_FINISH(3, "in mjit_cont_new");

    xfree(cont);
}

/* Finish work with continuation info. */
static void
finish_conts(void)
{
    struct mjit_cont *cont, *next;

    for (cont = first_cont; cont != NULL; cont = next) {
        next = cont->next;
        xfree(cont);
    }
}

/* Create unit for ISEQ. */
static void
create_unit(const rb_iseq_t *iseq)
{
    struct rb_mjit_unit *unit;

    unit = ZALLOC(struct rb_mjit_unit);
    if (unit == NULL)
        return;

    unit->id = mjit_current_unit_num++;
    unit->iseq = iseq;
    iseq->body->jit_unit = unit;
}

/* Set up field used_code_p for unit iseqs whose iseq on the stack of ec. */
static void
mark_ec_units(rb_execution_context_t *ec)
{
    const rb_iseq_t *iseq;
    const rb_control_frame_t *cfp;
    rb_control_frame_t *last_cfp = ec->cfp;
    const rb_control_frame_t *end_marker_cfp;
    uintptr_t i, size;

    if (ec->vm_stack == NULL)
        return;
    end_marker_cfp = RUBY_VM_END_CONTROL_FRAME(ec);
    size = end_marker_cfp - last_cfp;
    for (i = 0, cfp = end_marker_cfp - 1; i < size; i++, cfp = RUBY_VM_NEXT_CONTROL_FRAME(cfp)) {
        if (cfp->pc && (iseq = cfp->iseq) != NULL
            && imemo_type((VALUE) iseq) == imemo_iseq
            && (iseq->body->jit_unit) != NULL) {
            iseq->body->jit_unit->used_code_p = TRUE;
        }
    }
}

/* Unload JIT code of some units to satisfy the maximum permitted
   number of units with a loaded code.  */
static void
unload_units(void)
{
    rb_vm_t *vm = GET_THREAD()->vm;
    rb_thread_t *th = NULL;
    struct rb_mjit_unit_node *node, *next, *worst_node;
    struct mjit_cont *cont;
    int delete_num, units_num = mjit_active_units.length;

    /* For now, we don't unload units when ISeq is GCed. We should
       unload such ISeqs first here. */
    for (node = mjit_active_units.head; node != NULL; node = next) {
        next = node->next;
        if (node->unit->iseq == NULL) { /* ISeq is GCed. */
            free_unit(node->unit);
            remove_from_list(node, &mjit_active_units);
        }
    }

    /* Detect units which are in use and can't be unloaded. */
    for (node = mjit_active_units.head; node != NULL; node = node->next) {
        assert(node->unit != NULL && node->unit->iseq != NULL && node->unit->handle != NULL);
        node->unit->used_code_p = FALSE;
    }
    list_for_each(&vm->living_threads, th, vmlt_node) {
        mark_ec_units(th->ec);
    }
    for (cont = first_cont; cont != NULL; cont = cont->next) {
        mark_ec_units(cont->ec);
    }

    /* Remove 1/10 units more to decrease unloading calls.  */
    /* TODO: Calculate max total_calls in mjit_unit_queue and don't unload units
       whose total_calls are larger than the max. */
    delete_num = mjit_active_units.length / 10;
    for (; mjit_active_units.length > mjit_opts.max_cache_size - delete_num;) {
        /* Find one unit that has the minimum total_calls. */
        worst_node = NULL;
        for (node = mjit_active_units.head; node != NULL; node = node->next) {
            if (node->unit->used_code_p) /* We can't unload code on stack. */
                continue;

            if (worst_node == NULL || worst_node->unit->iseq->body->total_calls > node->unit->iseq->body->total_calls) {
                worst_node = node;
            }
        }
        if (worst_node == NULL)
            break;

        /* Unload the worst node. */
        verbose(2, "Unloading unit %d (calls=%lu)", worst_node->unit->id, worst_node->unit->iseq->body->total_calls);
        assert(worst_node->unit->handle != NULL);
        free_unit(worst_node->unit);
        remove_from_list(worst_node, &mjit_active_units);
    }
    verbose(1, "Too many JIT code -- %d units unloaded", units_num - mjit_active_units.length);
}

/* Add ISEQ to be JITed in parallel with the current thread.
   Unload some JIT codes if there are too many of them.  */
void
mjit_add_iseq_to_process(const rb_iseq_t *iseq)
{
    struct rb_mjit_unit_node *node;

    if (!mjit_enabled || mjit_pch_status == PCH_FAILED)
        return;

    iseq->body->jit_func = (mjit_func_t)NOT_READY_JIT_ISEQ_FUNC;
    create_unit(iseq);
    if (iseq->body->jit_unit == NULL)
        /* Failure in creating the unit.  */
        return;

    node = create_list_node(iseq->body->jit_unit);
    CRITICAL_SECTION_START(3, "in add_iseq_to_process");
    add_to_list(node, &mjit_unit_queue);
    if (mjit_active_units.length >= mjit_opts.max_cache_size) {
        unload_units();
    }
    verbose(3, "Sending wakeup signal to workers in mjit_add_iseq_to_process");
    rb_native_cond_broadcast(&mjit_worker_wakeup);
    CRITICAL_SECTION_FINISH(3, "in add_iseq_to_process");
}

/* For this timeout seconds, --jit-wait will wait for JIT compilation finish. */
#define MJIT_WAIT_TIMEOUT_SECONDS 60

/* Wait for JIT compilation finish for --jit-wait. This should only return a function pointer
   or NOT_COMPILED_JIT_ISEQ_FUNC. */
mjit_func_t
mjit_get_iseq_func(struct rb_iseq_constant_body *body)
{
    struct timeval tv;
    int tries = 0;
    tv.tv_sec = 0;
    tv.tv_usec = 1000;
    while (body->jit_func == (mjit_func_t)NOT_READY_JIT_ISEQ_FUNC) {
        tries++;
        if (tries / 1000 > MJIT_WAIT_TIMEOUT_SECONDS || mjit_pch_status == PCH_FAILED) {
            CRITICAL_SECTION_START(3, "in mjit_get_iseq_func to set jit_func");
            body->jit_func = (mjit_func_t)NOT_COMPILED_JIT_ISEQ_FUNC; /* JIT worker seems dead. Give up. */
            CRITICAL_SECTION_FINISH(3, "in mjit_get_iseq_func to set jit_func");
            if (mjit_opts.warnings || mjit_opts.verbose)
                fprintf(stderr, "MJIT warning: timed out to wait for JIT finish\n");
            break;
        }

        CRITICAL_SECTION_START(3, "in mjit_get_iseq_func for a client wakeup");
        rb_native_cond_broadcast(&mjit_worker_wakeup);
        CRITICAL_SECTION_FINISH(3, "in mjit_get_iseq_func for a client wakeup");
        rb_thread_wait_for(tv);
    }
    return body->jit_func;
}

extern VALUE ruby_archlibdir_path, ruby_prefix_path;

/* Initialize mjit_header_file, mjit_pch_file, mjit_libruby_pathflag. Return TRUE on success. */
static int
init_header_filename(void)
{
    int fd;
    /* Root path of the running ruby process. Equal to RbConfig::TOPDIR.  */
    VALUE basedir_val;
    const char *basedir;
    size_t baselen;
    char *p;
#ifdef _WIN32
    static const char libpathflag[] =
# ifdef _MSC_VER
        "-LIBPATH:"
# else
        "-L"
# endif
	;
    const size_t libpathflag_len = sizeof(libpathflag) - 1;
#endif

    basedir_val = ruby_prefix_path;
    basedir = StringValuePtr(basedir_val);
    baselen = RSTRING_LEN(basedir_val);

#ifndef LOAD_RELATIVE
    if (getenv("MJIT_SEARCH_BUILD_DIR")) {
        /* This path is not intended to be used on production, but using build directory's
           header file here because people want to run `make test-all` without running
           `make install`. Don't use $MJIT_SEARCH_BUILD_DIR except for test-all. */
        basedir = MJIT_BUILD_DIR;
        baselen = strlen(basedir);
    }
#endif

#ifndef _MSC_VER
    {
        /* A name of the header file included in any C file generated by MJIT for iseqs. */
        static const char header_name[] = MJIT_MIN_HEADER_NAME;
        const size_t header_name_len = sizeof(header_name) - 1;

        mjit_header_file = xmalloc(baselen + header_name_len + 1);
        p = append_str2(mjit_header_file, basedir, baselen);
        p = append_str2(p, header_name, header_name_len + 1);
        if ((fd = rb_cloexec_open(mjit_header_file, O_RDONLY, 0)) < 0) {
            verbose(1, "Cannot access header file: %s", mjit_header_file);
            xfree(mjit_header_file);
            mjit_header_file = NULL;
            return FALSE;
        }
        (void)close(fd);
    }

    mjit_pch_file = get_uniq_filename(0, MJIT_TMP_PREFIX "h", ".h.gch");
    if (mjit_pch_file == NULL)
        return FALSE;
#else
    {
        static const char pch_name[] = MJIT_PRECOMPILED_HEADER_NAME;
        const size_t pch_name_len = sizeof(pch_name) - 1;

        mjit_pch_file = xmalloc(baselen + pch_name_len + 1);
        p = append_str2(mjit_pch_file, basedir, baselen);
        p = append_str2(p, pch_name, pch_name_len + 1);
        if ((fd = rb_cloexec_open(mjit_pch_file, O_RDONLY, 0)) < 0) {
            verbose(1, "Cannot access precompiled header file: %s", mjit_pch_file);
            xfree(mjit_pch_file);
            mjit_pch_file = NULL;
            return FALSE;
        }
        (void)close(fd);
    }
#endif

#ifdef _WIN32
    basedir_val = ruby_archlibdir_path;
    basedir = StringValuePtr(basedir_val);
    baselen = RSTRING_LEN(basedir_val);
    mjit_libruby_pathflag = p = xmalloc(libpathflag_len + baselen + 1);
    p = append_str(p, libpathflag);
    p = append_str2(p, basedir, baselen);
    *p = '\0';
#endif

    return TRUE;
}

/* This is called after each fork in the child in to switch off MJIT
   engine in the child as it does not inherit MJIT threads.  */
void
mjit_child_after_fork(void)
{
    if (mjit_enabled) {
        verbose(3, "Switching off MJIT in a forked child");
        mjit_enabled = FALSE;
    }
    /* TODO: Should we initiate MJIT in the forked Ruby.  */
}

static enum rb_id_table_iterator_result
valid_class_serials_add_i(ID key, VALUE v, void *unused)
{
    rb_const_entry_t *ce = (rb_const_entry_t *)v;
    VALUE value = ce->value;

    if (!rb_is_const_id(key)) return ID_TABLE_CONTINUE;
    if (RB_TYPE_P(value, T_MODULE) || RB_TYPE_P(value, T_CLASS)) {
        mjit_add_class_serial(RCLASS_SERIAL(value));
    }
    return ID_TABLE_CONTINUE;
}

#ifdef _WIN32
UINT rb_w32_system_tmpdir(WCHAR *path, UINT len);
#endif

static char *
system_default_tmpdir(void)
{
    /* c.f. ext/etc/etc.c:etc_systmpdir() */
#ifdef _WIN32
    WCHAR tmppath[_MAX_PATH];
    UINT len = rb_w32_system_tmpdir(tmppath, numberof(tmppath));
    if (len) {
        int blen = WideCharToMultiByte(CP_UTF8, 0, tmppath, len, NULL, 0, NULL, NULL);
        char *tmpdir = xmalloc(blen + 1);
        WideCharToMultiByte(CP_UTF8, 0, tmppath, len, tmpdir, blen, NULL, NULL);
        tmpdir[blen] = '\0';
        return tmpdir;
    }
#elif defined _CS_DARWIN_USER_TEMP_DIR
    char path[MAXPATHLEN];
    size_t len = confstr(_CS_DARWIN_USER_TEMP_DIR, path, sizeof(path));
    if (len > 0) {
        char *tmpdir = xmalloc(len);
        if (len > sizeof(path)) {
            confstr(_CS_DARWIN_USER_TEMP_DIR, tmpdir, len);
        }
        else {
            memcpy(tmpdir, path, len);
        }
        return tmpdir;
    }
#endif
    return 0;
}

static int
check_tmpdir(const char *dir)
{
    struct stat st;

    if (!dir) return FALSE;
    if (stat(dir, &st)) return FALSE;
#ifndef S_ISDIR
#   define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
    if (!S_ISDIR(st.st_mode)) return FALSE;
#ifndef _WIN32
# ifndef S_IWOTH
#   define S_IWOTH 002
# endif
    if (st.st_mode & S_IWOTH) {
# ifdef S_ISVTX
        if (!(st.st_mode & S_ISVTX)) return FALSE;
# else
        return FALSE;
# endif
    }
    if (access(dir, W_OK)) return FALSE;
#endif
    return TRUE;
}

static char *
system_tmpdir(void)
{
    char *tmpdir;
# define RETURN_ENV(name) \
    if (check_tmpdir(tmpdir = getenv(name))) return get_string(tmpdir)
    RETURN_ENV("TMPDIR");
    RETURN_ENV("TMP");
    tmpdir = system_default_tmpdir();
    if (check_tmpdir(tmpdir)) return tmpdir;
    return get_string("/tmp");
# undef RETURN_ENV
}

/* Default permitted number of units with a JIT code kept in
   memory.  */
#define DEFAULT_CACHE_SIZE 1000
/* A default threshold used to add iseq to JIT. */
#define DEFAULT_MIN_CALLS_TO_ADD 5
/* Minimum value for JIT cache size.  */
#define MIN_CACHE_SIZE 10

/* Start MJIT worker. Return TRUE if worker is sucessfully started. */
static int
start_worker(void)
{
    extern void mjit_worker(void);

    mjit_stop_worker_p = FALSE;
    mjit_worker_stopped = FALSE;

    if (!rb_thread_create_mjit_thread(mjit_worker)) {
        mjit_enabled = FALSE;
        rb_native_mutex_destroy(&mjit_engine_mutex);
        rb_native_cond_destroy(&mjit_pch_wakeup);
        rb_native_cond_destroy(&mjit_client_wakeup);
        rb_native_cond_destroy(&mjit_worker_wakeup);
        rb_native_cond_destroy(&mjit_gc_wakeup);
        verbose(1, "Failure in MJIT thread initialization\n");
        return FALSE;
    }
    return TRUE;
}

/* Initialize MJIT.  Start a thread creating the precompiled header and
   processing ISeqs.  The function should be called first for using MJIT.
   If everything is successfull, MJIT_INIT_P will be TRUE.  */
void
mjit_init(struct mjit_options *opts)
{
    mjit_opts = *opts;
    mjit_enabled = TRUE;
    mjit_call_p = TRUE;

    /* Normalize options */
    if (mjit_opts.min_calls == 0)
        mjit_opts.min_calls = DEFAULT_MIN_CALLS_TO_ADD;
    if (mjit_opts.max_cache_size <= 0)
        mjit_opts.max_cache_size = DEFAULT_CACHE_SIZE;
    if (mjit_opts.max_cache_size < MIN_CACHE_SIZE)
        mjit_opts.max_cache_size = MIN_CACHE_SIZE;

    verbose(2, "MJIT: CC defaults to %s", CC_PATH);

    /* Initialize variables for compilation */
#ifdef _MSC_VER
    mjit_pch_status = PCH_SUCCESS; /* has prebuilt precompiled header */
#else
    mjit_pch_status = PCH_NOT_READY;
#endif
    mjit_cc_path = CC_PATH;

    mjit_tmp_dir = system_tmpdir();
    verbose(2, "MJIT: mjit_tmp_dir is %s", mjit_tmp_dir);

    if (!init_header_filename()) {
        mjit_enabled = FALSE;
        verbose(1, "Failure in MJIT header file name initialization\n");
        return;
    }

    init_list(&mjit_unit_queue);
    init_list(&mjit_active_units);
    init_list(&mjit_compact_units);

    /* Initialize mutex */
    rb_native_mutex_initialize(&mjit_engine_mutex);
    rb_native_cond_initialize(&mjit_pch_wakeup);
    rb_native_cond_initialize(&mjit_client_wakeup);
    rb_native_cond_initialize(&mjit_worker_wakeup);
    rb_native_cond_initialize(&mjit_gc_wakeup);

    /* Initialize class_serials cache for compilation */
    mjit_valid_class_serials = rb_hash_new();
    rb_obj_hide(mjit_valid_class_serials);
    rb_gc_register_mark_object(mjit_valid_class_serials);
    mjit_add_class_serial(RCLASS_SERIAL(rb_cObject));
    mjit_add_class_serial(RCLASS_SERIAL(CLASS_OF(rb_vm_top_self())));
    if (RCLASS_CONST_TBL(rb_cObject)) {
        rb_id_table_foreach(RCLASS_CONST_TBL(rb_cObject), valid_class_serials_add_i, NULL);
    }

    /* Initialize worker thread */
    start_worker();
}

static void
stop_worker(void)
{
    rb_execution_context_t *ec = GET_EC();

    mjit_stop_worker_p = TRUE;
    while (!mjit_worker_stopped) {
        verbose(3, "Sending cancel signal to worker");
        CRITICAL_SECTION_START(3, "in stop_worker");
        rb_native_cond_broadcast(&mjit_worker_wakeup);
        CRITICAL_SECTION_FINISH(3, "in stop_worker");
        RUBY_VM_CHECK_INTS(ec);
    }
}

/* Stop JIT-compiling methods but compiled code is kept available. */
VALUE
mjit_pause(int wait_p)
{
    if (!mjit_enabled) {
        rb_raise(rb_eRuntimeError, "MJIT is not enabled");
    }
    if (mjit_worker_stopped) {
        return Qfalse;
    }

    /* Flush all queued units with no option or `wait: true` */
    if (wait_p) {
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 1000;

        while (mjit_unit_queue.length > 0) {
            CRITICAL_SECTION_START(3, "in mjit_pause for a worker wakeup");
            rb_native_cond_broadcast(&mjit_worker_wakeup);
            CRITICAL_SECTION_FINISH(3, "in mjit_pause for a worker wakeup");
            rb_thread_wait_for(tv);
        }
    }

    stop_worker();
    return Qtrue;
}

/* Restart JIT-compiling methods after mjit_pause. */
VALUE
mjit_resume(void)
{
    if (!mjit_enabled) {
        rb_raise(rb_eRuntimeError, "MJIT is not enabled");
    }
    if (!mjit_worker_stopped) {
        return Qfalse;
    }

    if (!start_worker()) {
        rb_raise(rb_eRuntimeError, "Failed to resume MJIT worker");
    }
    return Qtrue;
}

/* Finish the threads processing units and creating PCH, finalize
   and free MJIT data.  It should be called last during MJIT
   life.  */
void
mjit_finish(void)
{
    if (!mjit_enabled)
        return;

    /* Wait for pch finish */
    verbose(2, "Stopping worker thread");
    CRITICAL_SECTION_START(3, "in mjit_finish to wakeup from pch");
    /* As our threads are detached, we could just cancel them.  But it
       is a bad idea because OS processes (C compiler) started by
       threads can produce temp files.  And even if the temp files are
       removed, the used C compiler still complaint about their
       absence.  So wait for a clean finish of the threads.  */
    while (mjit_pch_status == PCH_NOT_READY) {
        verbose(3, "Waiting wakeup from make_pch");
        rb_native_cond_wait(&mjit_pch_wakeup, &mjit_engine_mutex);
    }
    CRITICAL_SECTION_FINISH(3, "in mjit_finish to wakeup from pch");

    /* Stop worker */
    stop_worker();

    rb_native_mutex_destroy(&mjit_engine_mutex);
    rb_native_cond_destroy(&mjit_pch_wakeup);
    rb_native_cond_destroy(&mjit_client_wakeup);
    rb_native_cond_destroy(&mjit_worker_wakeup);
    rb_native_cond_destroy(&mjit_gc_wakeup);

#ifndef _MSC_VER /* mswin has prebuilt precompiled header */
    if (!mjit_opts.save_temps)
        remove_file(mjit_pch_file);

    xfree(mjit_header_file); mjit_header_file = NULL;
#endif
    xfree(mjit_tmp_dir); mjit_tmp_dir = NULL;
    xfree(mjit_pch_file); mjit_pch_file = NULL;

    mjit_call_p = FALSE;
    free_list(&mjit_unit_queue);
    free_list(&mjit_active_units);
    free_list(&mjit_compact_units);
    finish_conts();

    mjit_enabled = FALSE;
    verbose(1, "Successful MJIT finish");
}

void
mjit_mark(void)
{
    struct rb_mjit_unit_node *node;
    if (!mjit_enabled)
        return;
    RUBY_MARK_ENTER("mjit");
    CRITICAL_SECTION_START(4, "mjit_mark");
    for (node = mjit_unit_queue.head; node != NULL; node = node->next) {
        if (node->unit->iseq) { /* ISeq is still not GCed */
            VALUE iseq = (VALUE)node->unit->iseq;
            CRITICAL_SECTION_FINISH(4, "mjit_mark rb_gc_mark");

            /* Don't wrap critical section with this. This may trigger GC,
               and in that case mjit_gc_start_hook causes deadlock. */
            rb_gc_mark(iseq);

            CRITICAL_SECTION_START(4, "mjit_mark rb_gc_mark");
        }
    }
    CRITICAL_SECTION_FINISH(4, "mjit_mark");
    RUBY_MARK_LEAVE("mjit");
}

/* A hook to update mjit_valid_class_serials. */
void
mjit_add_class_serial(rb_serial_t class_serial)
{
    if (!mjit_enabled)
        return;

    /* Do not wrap CRITICAL_SECTION here. This function is only called in main thread
       and guarded by GVL, and `rb_hash_aset` may cause GC and deadlock in it. */
    rb_hash_aset(mjit_valid_class_serials, LONG2FIX(class_serial), Qtrue);
}

/* A hook to update mjit_valid_class_serials. */
void
mjit_remove_class_serial(rb_serial_t class_serial)
{
    if (!mjit_enabled)
        return;

    CRITICAL_SECTION_START(3, "in mjit_remove_class_serial");
    rb_hash_delete_entry(mjit_valid_class_serials, LONG2FIX(class_serial));
    CRITICAL_SECTION_FINISH(3, "in mjit_remove_class_serial");
}
