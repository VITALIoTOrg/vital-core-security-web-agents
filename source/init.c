/**
 * The contents of this file are subject to the terms of the Common Development and
 * Distribution License (the License). You may not use this file except in compliance with the
 * License.
 *
 * You can obtain a copy of the License at legal/CDDLv1.0.txt. See the License for the
 * specific language governing permission and limitations under the License.
 *
 * When distributing Covered Software, include this CDDL Header Notice in each file and include
 * the License file at legal/CDDLv1.0.txt. If applicable, add the following below the CDDL
 * Header, with the fields enclosed by brackets [] replaced by your own identifying
 * information: "Portions copyright [year] [name of copyright owner]".
 *
 * Copyright 2014 - 2015 ForgeRock AS.
 */

#include "platform.h"
#include "am.h"
#include "net_client.h"
#include "utility.h"

#ifdef _WIN32

struct am_main_init {
    HANDLE id;
    int error;
};

struct am_main_init init = {
    NULL,
    AM_ERROR
};

static void am_main_create(int id) {
    SECURITY_DESCRIPTOR sec_descr;
    SECURITY_ATTRIBUTES sec_attr, *sec = NULL;
    
    if (InitializeSecurityDescriptor(&sec_descr, SECURITY_DESCRIPTOR_REVISION) &&
            SetSecurityDescriptorDacl(&sec_descr, TRUE, (PACL) NULL, FALSE)) {
        sec_attr.nLength = sizeof (SECURITY_ATTRIBUTES);
        sec_attr.lpSecurityDescriptor = &sec_descr;
        sec_attr.bInheritHandle = TRUE;
        sec = &sec_attr;
    }
    
    init.id = CreateMutexA(sec, FALSE, get_global_name(AM_GLOBAL_PREFIX"am_main_init_lock", id));
    if (init.id == NULL && GetLastError() == ERROR_ACCESS_DENIED) {
        init.id = OpenMutexA(SYNCHRONIZE, TRUE, get_global_name(AM_GLOBAL_PREFIX"am_main_init_lock", id));
    }
}

static void am_main_destroy() {
    CloseHandle(init.id);
    init.id = NULL;
    init.error = AM_ERROR;
}

static void am_main_init_timed_lock() {
    DWORD status = WaitForSingleObject(init.id, 100);
    switch (status) {
        case WAIT_OBJECT_0:
            init.error = AM_SUCCESS;
            break;
        case WAIT_ABANDONED:
            init.error = AM_EAGAIN;
            break;
        case WAIT_TIMEOUT:
            init.error = AM_ETIMEDOUT;
            break;
        default:
            init.error = AM_ERROR;
            break;
    }
}

static void am_main_init_unlock() {
    ReleaseMutex(init.id);
    init.error = AM_SUCCESS;
}

#endif

/*
 * Initialising the agent:
 *
 * On Unix, where fork is used by the webserver to spawn a new child (worker) process, there is a
 * main-process-init and a child-process init. Whereas on windows there is no main process (fork is not
 * available) - all processes are equal and must be dealt with appropriately. Thus the difference in
 * init calls to get agent bootstrapped in different environments, i.e. for unix (and it's variants) we
 * call this function am_init.  For Windows, call am_init_worker.
 */
int am_init(int id, int (*init_status_cb)(int)) {
    int rv = AM_SUCCESS;
#ifndef _WIN32
    am_net_init();
    am_log_init(id, AM_SUCCESS);
    am_configuration_init(id);
    am_audit_init(id);
    am_audit_processor_init();
    am_url_validator_init();
    rv = am_cache_init(id);
    am_worker_pool_init(init_status_cb);
#endif
    return rv;
}

int am_init_worker(int id) {
#ifdef _WIN32
    am_net_init();
    am_main_create(id);
    am_main_init_timed_lock();
    am_log_init_worker(id, init.error);
    am_configuration_init(id);
    am_audit_init(id);
    if (init.error == AM_SUCCESS || init.error == AM_EAGAIN) {
        am_audit_processor_init();
        am_url_validator_init();
    }
    am_cache_init(id);
#endif
    am_worker_pool_init(NULL);
    return 0;
}

int am_shutdown(int id) {
    am_url_validator_shutdown();
    am_audit_processor_shutdown();
    am_audit_shutdown();
    am_cache_shutdown();
    am_configuration_shutdown();
    am_log_shutdown(id);
#ifdef _WIN32
    am_main_destroy();
#else
    am_worker_pool_shutdown();
    am_net_shutdown();
#endif
    return 0;
}

int am_re_init_worker() {
#ifdef _WIN32
    am_main_init_timed_lock();
    if (init.error == AM_SUCCESS || init.error == AM_EAGAIN) {
        am_log_re_init(AM_RETRY_ERROR);
        am_audit_processor_init();
        am_url_validator_init();
    }
#endif
    return 0;
}

int am_shutdown_worker() {
#ifdef _WIN32
    am_main_init_unlock();
#endif
    am_worker_pool_shutdown();
#ifdef _WIN32
    am_net_shutdown();
#endif
    return 0;
}

#ifndef _WIN32
/*
 * get shared memory name, as opened by am_shm_create
 */
static am_bool_t get_shm_name(const char *root, int id, char *buffer, size_t buffer_sz) {
    char *global_name = get_global_name(root, id);
    int sz;
    
    if (global_name == NULL)
        return AM_FALSE;
    
    sz = snprintf(buffer, buffer_sz,
#ifdef __sun
             "/%s_s"
#else
             "%s_s"
#endif
             , global_name);

    return 0 <= sz && sz < buffer_sz;
}

/*
 * get shared memory name, as opened by am_log_init
 */
static am_bool_t get_log_shm_name(int id, char *buffer, size_t buffer_sz) {
    int sz = snprintf(buffer, buffer_sz,
#ifdef __sun
             "/am_log_%d"
#else
             AM_GLOBAL_PREFIX"am_log_%d"
#endif
             , id);
    
    return 0 <= sz && sz < buffer_sz;
}

static am_bool_t unlink_shm(char *shm_name, void (*log_cb)(void *arg, char *name, int error), void *cb_arg) {
    errno = 0;
    if (shm_unlink(shm_name) == 0) {
        // warn: shared memory was present but successfully cleared
        log_cb(cb_arg, shm_name, 0);
    } else if (errno != ENOENT) {
        // failure: shared memory was not cleared
        log_cb(cb_arg, shm_name, errno);
        return AM_FALSE;
    }
    return AM_TRUE;
}

/*
 * get semaphore name as opened by am_instance_init_init
 */
static am_bool_t get_config_sem_name(int id, char *buffer, size_t buffer_sz) {
   char *global_name = get_global_name(
#ifdef __sun
                   "/"AM_CONFIG_INIT_NAME
#else
                   AM_CONFIG_INIT_NAME
#endif
                   , id);

    if (global_name == NULL)
        return AM_FALSE;

    int sz = snprintf(buffer, buffer_sz, "%s", global_name);
    return 0 <= sz && sz < buffer_sz;
}

static am_bool_t unlink_sem(char *sem_name, void (*log_cb)(void *arg, char *name, int error), void *cb_arg) {
    errno = 0;
    if (sem_unlink(sem_name) == 0) {
        // warn: semaphore was present but successfully cleared
        log_cb(cb_arg, sem_name, 0);
    } else if (errno != ENOENT
#ifdef __APPLE__
            && errno != EINVAL
#endif
            ) {
        // failure: semaphore was not cleared
        log_cb(cb_arg, sem_name, errno);
        return AM_FALSE;
    }
    return AM_TRUE;
}
#endif

/*
 * Remove all shared memory and semaphore resources that might be left open if the agent terminates
 * abnormally
 */
am_status_t am_remove_shm_and_locks(int id, void (*log_cb)(void *arg, char *name, int error), void *cb_arg) {
#ifdef _WIN32
    return AM_SUCCESS;
#else
    am_status_t status = AM_SUCCESS;
    char name [AM_PATH_SIZE];

    if (!(get_shm_name(AM_AUDIT_SHM_NAME, id, name, sizeof (name)) && unlink_shm(name, log_cb, cb_arg))) {
        status = AM_ERROR;
    }

    if (!(get_shm_name(AM_CACHE_SHM_NAME, id, name, sizeof (name)) && unlink_shm(name, log_cb, cb_arg))) {
        status = AM_ERROR;
    }

    if (!(get_shm_name(AM_CONFIG_SHM_NAME, id, name, sizeof (name)) && unlink_shm(name, log_cb, cb_arg))) {
        status = AM_ERROR;
    }

    if (!(get_log_shm_name(id, name, sizeof (name)) && unlink_shm(name, log_cb, cb_arg))) {
        status = AM_ERROR;
    }

    if (!(get_config_sem_name(id, name, sizeof (name)) && unlink_sem(name, log_cb, cb_arg))) {
        status = AM_ERROR;
    }
    
    return status;
#endif
}
